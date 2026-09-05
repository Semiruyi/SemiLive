# 视频编码阶段设计

本文定义 Publisher 从 `CapturedVideoFrameStore` 消费 CPU BGRA 帧、完成图像预处理与 H.264
编码、再向 `EncodedVideoAccessUnitQueue` 发布 Access Unit 的模块边界、线程模型、背压、停止语义
和验证方法。它是 [Publisher 音视频设计总览](overview.md) 中 Video Encode Thread 的详细设计，
上游采集语义见 [VideoCaptureWorker 设计](video-capture-worker.md)。

## 1. 目标与边界

`VideoEncoderWorker` 负责：

- 拥有一个与 Worker 对象同寿命的常驻线程；
- 保证 `VideoEncoderBackend` 只在该线程中打开、调用、flush 和关闭；
- 响应 `CapturedVideoFrameStoreNotEmpty` 与 `EncodedVideoAccessUnitQueueNotFull` 边界通知；
- 从 `CapturedVideoFrameSource` 消费最新的未编码 BGRA 帧；
- 调用 Backend 完成等比缩放、黑边填充、YUV420P 转换和 H.264 编码；
- 保留 Backend 尚未产生输出所需的编码状态，并按顺序发布完整 Annex-B Access Unit；
- AU Queue 满时保留已经编码的输出并停止消费新采集帧；
- 在正常停止时排空输入并 flush，在故障停止时有界地放弃剩余工作；
- 上报第一个运行期致命错误，并提供线程安全的状态与统计快照。

`VideoEncoderWorker` 不负责：

- 采集桌面、生成视频 PTS 或决定采集帧率；
- 清理上游 FrameStore 或下游 AU Queue；
- 拆分 H.264 NAL、生成 RTP 包、发送 UDP 或保存正式诊断文件；
- 根据网络反馈动态修改码率、帧率或请求关键帧；
- 在首版中创建独立的图像预处理线程；
- 暴露 FFmpeg 的 `AVFrame`、`AVPacket` 或 `AVCodecContext`。

Main 不直接访问 Worker。`PublisherComposition` 创建并持有 Worker，`PublisherController` 是其
唯一控制方。Backend 的所有权转移给 Worker；FrameSource、AU Sink 与 Notifier 必须比 Worker
活得更久。

## 2. 组件边界

### 2.1 一个公开 Backend 边界

后端契约层只定义一个 `VideoEncoderBackend`：

```text
CapturedVideoFrame（CPU BGRA）
-> VideoEncoderBackend
-> 0..N EncodedVideoAccessUnit（Annex-B H.264）
```

不定义公开的 `VideoFrameProcessor` 或通用 `ProcessedVideoFrame`。把 YUV420P 暴露为共享模型会
迫使接口在以下方案中选择其一：复制整帧、公开危险的非拥有 View，或泄漏 `AVFrame`。首版中
预处理与编码没有队列边界，单独的公开抽象不能提供与其复杂度相称的替换价值。

这不表示实现集中在一个大类或源文件。FFmpeg 基础设施内部按职责拆分：

```text
FfmpegH264EncoderBackend
├── VideoPlacement             纯整数缩放区域计算，不依赖 FFmpeg
├── SwsFrameConverter          管理 SwsContext、YUV AVFrame 和缓冲复用
└── FfmpegH264Encoder          管理 AVCodecContext、send/receive 和 flush
```

`VideoPlacement` 可以作为不依赖基础设施的纯值计算单独测试。其余辅助类属于 FFmpeg 实现细节，可以在
基础设施测试中分别验证，但不成为 Worker 的注入依赖。`FfmpegH264EncoderBackend` 是唯一实现
视频编码 Backend 契约的对象，保证 FFmpeg 帧引用和编码延迟完全封装在同一资源所有者中。

### 2.2 首版线程边界

首版执行拓扑为：

```text
Video Capture Thread
-> CapturedVideoFrameStore(capacity = 2, replace oldest)
-> Video Encode Thread
   -> BGRA scale/letterbox/YUV420P
   -> libx264 encode
-> EncodedVideoAccessUnitQueue(capacity = 4, reject when full)
-> Video Send Thread
```

Capture、Encode 和后续 Send 已经可以并行。Video Encode Thread 顺序调用颜色转换与编码 API，
但 libx264 可以在 Backend 内使用自己的受控线程；“一个 Encode Worker”不等于整个编码过程只
使用一个 CPU 核。

首版不在转换和编码之间增加线程与 YUV Queue。额外边界会引入 YUV 缓冲所有权、内存水位、
第二套背压、停止次序和线程切换，也可能与编码器内部线程竞争 CPU。是否增加
`VideoPreprocessWorker` 必须由性能报告决定，不能只根据理论并行度决定。

满足以下现象时才进入拆线程评估：

- 目标机器上预处理加编码的持续吞吐低于 30 fps；
- FrameStore 持续替换而不是只在瞬时负载下偶发替换；
- 分阶段测量证明预处理占有可观成本，且与编码重叠后预计能提高吞吐；
- 降低不必要的转换成本、确认 libx264 配置和内部线程后仍不达标。

如果未来拆分，YUV Queue 必须容量受限且仍允许丢弃尚未送入编码器的帧；已经进入 H.264 参考链
或已经产生的 AU 仍不得随意丢弃。该演进不改变 Capture 与 RTP 边界。

## 3. Backend 契约草案

`VideoDimensions`、`FrameRate`、`CapturedVideoFrame` 和 `EncodedVideoAccessUnit` 属于无行为的
共享模型；编码配置、启动信息、批次诊断、结构化错误和虚接口属于编码契约。契约只能依赖
共享模型和标准库，不得依赖领域模块或 FFmpeg 基础设施。

### 3.1 配置与启动信息

```cpp
struct VideoDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct VideoEncoderConfig {
    model::VideoDimensions output{1920, 1080};
    model::FrameRate frame_rate{30, 1};
    std::uint64_t target_bit_rate = 4'000'000;
    std::uint32_t gop_size = 60;
};

struct VideoEncoderInfo {
    model::VideoDimensions output;
    model::FrameRate frame_rate;
    std::uint64_t target_bit_rate = 0;
    std::uint32_t gop_size = 0;
    std::uint32_t maximum_delayed_frames = 0;
    std::string encoder_name;
};
```

`VideoEncoderConfig` 只表达视频轨道需要的通用约束，不放入 `AVPixelFormat`、`AVRational`、x264
私有字符串或线程对象。首版 Backend 固定输出 YUV420P、Annex-B H.264、无 B 帧，并应用
`ultrafast` 与 `zerolatency`；这些是实现选择和会话启动校验，不需要让 Controller 了解 FFmpeg。

输出宽高必须非零、为偶数且在 FFmpeg 可表示范围内；帧率分子和分母、码率及 GOP 必须大于
零。配置在一次会话内不可修改，需要改变时停止并开始新会话。输入尺寸来自每个
`BgraFrameBuffer`，不写入会话配置。

`VideoEncoderInfo` 返回 Backend 实际打开的编码器、有效参数和内部最多保留的输入帧数，便于
启动日志、内存上界、集成测试和性能报告确认没有静默回退。Backend 必须保证延迟输入数量不
超过 `maximum_delayed_frames`，违反时返回内部错误；找不到 `libx264` 或实际参数不满足基线时，
启动失败而不是切换到另一种编码器。

### 3.2 编码结果

```cpp
struct VideoEncodeBatch {
    std::vector<model::EncodedVideoAccessUnit> access_units;
    std::chrono::nanoseconds preprocessing_time{};
    std::chrono::nanoseconds codec_time{};
};
```

一次输入允许产生零个或多个 AU。即使首版无 B 帧和零延迟调优通常表现为一帧对应一个输出，
契约也不把该实现特征固化为永久假设。Backend 每次输入后持续 receive，直到编码器报告需要更多
输入；`flush()` 持续 receive 到编码器结束。

分阶段耗时由 Backend 在实际操作边界内测量并通过批次诊断信息返回。Worker 同时测量完整调用
耗时。`preprocessing_time` 和 `codec_time` 不进入媒体对象，也不影响时间戳；它们用于判断未来
是否值得拆分预处理线程。

### 3.3 错误与接口

```cpp
enum class VideoEncoderOperation {
    Open,
    ValidateInput,
    CalculatePlacement,
    ConvertFrame,
    SendFrame,
    ReceivePacket,
    Flush,
    Close,
};

struct VideoEncoderIssue {
    VideoEncoderOperation operation;
    std::int64_t native_code = 0;
    std::string message;
};

class VideoEncoderBackend {
public:
    virtual ~VideoEncoderBackend() = default;

    [[nodiscard]] virtual std::expected<VideoEncoderInfo, VideoEncoderIssue>
    open(const VideoEncoderConfig& config) = 0;

    [[nodiscard]] virtual std::expected<VideoEncodeBatch, VideoEncoderIssue>
    encode(const model::CapturedVideoFrame& frame) = 0;

    [[nodiscard]] virtual std::expected<VideoEncodeBatch, VideoEncoderIssue>
    flush() = 0;

    virtual void close() noexcept = 0;
};
```

这是语义草案，不要求实现逐字照搬。约束如下：

- 同一对象同一时刻最多打开一个会话；
- `open()`、`encode()`、`flush()` 和 `close()` 必须全部由同一 Worker 线程调用；
- `encode()` 返回前不再需要调用方保持输入对象本身存活，但可以在内部保留已引用的 FFmpeg
  YUV 帧与轻量 metadata；
- `flush()` 每次会话最多成功执行一次，之后只允许 `close()`；
- `close()` 幂等且不抛异常，可以关闭未 flush、部分打开或发生错误的会话；
- 配置错误和运行错误都返回结构化 Issue，异常不得越过 Worker 线程入口；
- Fake Backend 必须能够脚本化零输出、单输出、多输出、延迟输出、flush 输出和失败。

## 4. 图像预处理

### 4.1 输入校验

每次编码前验证：

- `frame.image` 非空；
- `width`、`height` 和 `stride` 非零；
- `stride >= width * 4`，所有乘法检查溢出；
- BGRA buffer 至少覆盖 `stride * height` 字节；
- `presentation_time` 非负且映射后的 90 kHz PTS 严格递增。

无效输入表示上游契约或内部状态被破坏，属于致命编码错误，不能跳过后继续使用可能已经不一致
的参考链。

### 4.2 等比缩放与黑边

`VideoPlacement` 使用整数运算计算能够完整放入固定画布的最大缩放矩形：

```cpp
struct VideoPlacement {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
```

规则如下：

- 保留完整输入，不裁剪、不拉伸；
- 缩放宽高向下调整为非零偶数，以满足 YUV420P 色度采样；
- `x`、`y` 也调整为偶数，左右或上下黑边因此最多相差 2 像素；
- 缩放矩形不能超出输出画布；
- 每帧先把完整 YUV 画布填黑，再把缩放结果写入矩形；
- 横屏、竖屏、超宽、超高和与输出同比例输入使用同一算法。

需要单元测试输入和输出同比例、4:3、竖屏、极端宽高比、奇数尺寸、最小有效尺寸以及乘法溢出
边界。测试比较明确的矩形数值和边界，不通过目测图片判断算法。

### 4.3 像素和色彩约束

首版固定：

| 项目 | 规则 |
|---|---|
| 输入 | CPU BGRA，忽略 Alpha |
| 缩放 | libswscale bilinear |
| 输出 | YUV420P 8-bit |
| 色彩矩阵 | BT.709 |
| 范围 | limited / MPEG range |
| 黑色 | Y=16、U=128、V=128 |
| Sample Aspect Ratio | 1:1 |

SwsContext 的输入尺寸变化时可以重建，YUV 输出画布和 H.264 编码器保持固定，因此 DXGI 恢复后
出现新的桌面尺寸不会改变已打开的视频轨道。转换失败不得复用上一帧 YUV 结果继续编码。

Backend 复用可写的 YUV `AVFrame`。若编码器仍持有上一帧引用，先通过 FFmpeg 的引用计数和
可写检查取得安全缓冲，不能覆盖编码器仍可能读取的内存。该生命周期完全留在 Backend 内部。

## 5. 编码与时间戳

### 5.1 H.264 基线

首版真实 Backend 明确请求 FFmpeg `libx264`：

| 参数 | 值 |
|---|---:|
| 输出尺寸 | 1920 x 1080 |
| 帧率 | 30/1 |
| time base | 1/90000 |
| 目标码率 | 4 Mbps |
| GOP | 60 个编码输入帧 |
| B 帧 | 0 |
| preset | ultrafast |
| tune | zerolatency |
| 输出格式 | Annex-B |
| SPS/PPS | IDR 前带内重复 |

不设置 `AV_CODEC_FLAG_GLOBAL_HEADER`，并在打开后验证有效像素格式、time base 和无 B 帧约束。
场景切换可以额外产生关键帧，但相邻 IDR 的最大编码帧数不能超过 GOP 基线。硬件编码和动态
码率调整不属于首版。

### 5.2 PTS 和 metadata 关联

每个输入帧使用已有整数函数映射：

```text
encoder_pts = media_time_to_clock_ticks(presentation_time, 90000)
```

不得把 `captured_at`、Worker 实际开始处理的时刻或连续编码计数当成 PTS。采集帧被 FrameStore
替换时，后续 PTS 可以存在合法缺口；编码器必须接受严格递增但不要求连续的输入时间戳。

Backend 为已经送入编码器但尚未输出的帧保存：

```text
encoder_pts -> presentation_time, source_sequence, captured_at
```

收到 AVPacket 后使用 packet PTS 找回 metadata，构造 `EncodedVideoAccessUnit`。输出继续保留原始
纳秒 `presentation_time`，不能把 90 kHz PTS 再换算回纳秒，以免发生第二次舍入。首版无 B 帧，
编码顺序与呈现顺序一致；如果输出缺少 PTS、找不到对应输入或顺序倒退，视为致命 Backend
错误。

一个成功 AU 必须：

- `annex_b` 非空且包含同一显示时刻的完整 Access Unit；
- `key_frame` 来自编码器输出标志而不是通过猜测 NAL 类型生成；
- `presentation_time`、`source_sequence` 和 `captured_at` 与对应输入完全一致；
- 不引用下一次 `encode()` 或 `close()` 会失效的 AVPacket 内存。

## 6. Worker 生命周期与控制

### 6.1 两层生命周期

Worker 构造时启动常驻 `std::jthread`，构造返回时线程已经完成入口初始化并处于 `Idle`；构造
本身不打开编码器。同一对象可以顺序执行多次会话，但一次最多打开一个 Backend：

```text
Idle -> Starting -> Running -> Draining -> Idle
  ^        |          |            |
  |        |          +-> Failed --+
  +--------+
   启动失败
```

与 VideoCaptureWorker 一致，公开控制接口同步等待，内部使用 typed command queue 与
promise/future，把所有 Backend 操作留在 Worker 线程。`std::jthread` stop request 只用于永久
关闭模块。

### 6.2 控制草案

```cpp
enum class VideoEncoderStopMode {
    Drain,
    Abort,
};

class VideoEncoderWorker {
public:
    [[nodiscard]] virtual std::expected<VideoEncoderStarted,
                                                VideoEncoderWorkerIssue>
    start(VideoEncoderSessionConfig config) = 0;

    virtual void stop(VideoEncoderStopMode mode) = 0;

    [[nodiscard]] virtual VideoEncoderWorkerState state() const noexcept = 0;
    [[nodiscard]] virtual VideoEncoderWorkerStats stats() const noexcept = 0;
};
```

`start()` 只接受 `Idle`，在 Worker 线程打开 Backend，成功后返回实际参数并进入 `Running`。
启动前 Controller 保证 FrameStore 和 AU Queue 为空。Worker 不擅自清理跨模块资源。

两种停止模式不可合并：

- `Drain` 用于正常停止。Controller 已先停止 Capture，Sender 仍在消费；Encoder 排空
  FrameStore、flush Backend，并等待所有 pending AU 被 AU Queue 接受后回到 `Idle`；
- `Abort` 用于任一模块失败或装配回滚。Encoder 立即停止消费，放弃未发布的内部 AU，不
  flush，关闭 Backend 并回到 `Idle`；Controller 随后统一 clear 两个领域资源。

如果只有无条件 drain，下游 Sender 已失败且 AU Queue 已满时，同步 `stop()` 可能永久等待；如果
只有 abort，正常结束会截断编码器中已经接受的帧。因此停止意图必须由 Controller 明确传入。

析构使用独立的永久关闭路径，语义等同安全 abort，不通过可能分配命令状态的公开 `stop()`。

## 7. 运行循环与背压

Worker 每次被控制命令、FrameStore NotEmpty 或 AU Queue NotFull 唤醒后，按以下优先级处理：

```text
1. 永久关闭或控制命令
2. 按顺序提交 pending AU
3. 若仍有 pending AU，等待 AU Queue NotFull
4. 若 Running，从 FrameStore 取一个 CapturedVideoFrame
5. 调用 Backend encode，追加本批次产生的 AU
6. 返回第 2 步
```

`encode()` 可能一次返回多个 AU，因此内部使用有界语义的 `deque`，而不是假设只有一个
`optional<EncodedVideoAccessUnit>`。Backend 的最大延迟受首版无 B 帧和 zerolatency 配置限制；
Worker 每次调用后立即取尽当前输出，Backend 延迟输入不超过启动时报告的上限，因此内部
metadata 和 pending 数量不会随会话时长无界增长。

AU Queue 满时，已经编码的 AU 不能丢弃。Worker 保留 pending，停止从 FrameStore 取帧并等待
`EncodedVideoAccessUnitQueueNotFull`：

```text
AU Queue 满
-> Encoder 停止消费 BGRA
-> FrameStore 开始替换最旧未编码帧
-> 上游继续保持最新画面
-> 内存和实时延迟保持有界
```

FrameStore 中被替换的帧从未进入编码器参考链，因此可以安全丢弃。Worker 观察相邻输入
`sequence` 的差值并记录 gap，但不补帧、不修改原始 PTS，也不因为 gap 自动请求 IDR。

Notifier 回调只设置 hint 并唤醒 Worker 条件变量，不能在发布通知的线程直接调用 Backend。
Worker 每次唤醒都重新查询资源真实状态，因此允许通知合并、重复或早于实际等待到达。

## 8. 错误传播

启动阶段 Backend 打开失败由 `start()` 返回，Worker 清理部分资源并回到 `Idle`。进入
`Running` 后发生以下情况属于致命错误：

- 输入 BGRA 布局无效或 PTS 不严格递增；
- placement、转换、send、receive 或 metadata 匹配失败；
- AU Sink 抛出异常或 Worker 内部状态不一致；
- flush 在正常 Draining 阶段失败。

首个运行错误切换到 `Failed`，停止继续读取 FrameStore，关闭 Backend，并发送一次：

```cpp
struct VideoEncoderWorkerFailed {
    VideoEncoderWorkerIssue issue;
};
```

Controller 收到后使整个发布会话失败，并对其他模块使用 abort 路径。普通背压、上游 sequence
gap 和一次输入暂时没有编码输出不是错误。

## 9. 统计与性能判定

`VideoEncoderWorkerStats` 至少提供：

- 本会话实际编码参数；
- 从 FrameStore 取出的输入帧数；
- 输入 sequence gap 次数及缺失帧总数；
- Backend 零输出、单输出和多输出调用次数；
- 产生、提交和 pending 的 AU 数量；
- 关键帧数、H.264 总字节数和估算码率；
- 预处理、codec 调用及完整 Backend 调用的累计、平均和最大耗时；
- AU Queue Full 次数、累计等待时间和最大等待时间；
- 采集完成到编码完成的累计、平均和最大延迟；
- drain 输入数、flush AU 数、abort 放弃的输入与内部 AU 数；
- 致命错误数和最后一个 Issue。

统计更新只能由 Worker 线程写入，`stats()` 在锁内返回快照。不要为每帧输出普通日志；状态
切换、首个错误和周期聚合统计才进入日志系统。

30 fps 的平均帧预算约为 33.33 ms。偶发超过预算并由 FrameStore 替换旧帧是实时链路的保护
机制；持续吞吐低于目标、持续 sequence gap 或不断增长的采集到编码延迟才表示容量不足。是否
拆分预处理线程必须引用可复现的阶段耗时和丢帧报告。

## 10. 测试与验收

### 10.1 单元测试

- VideoPlacement 的同比例、横向黑边、纵向黑边、奇数尺寸和溢出边界；
- Worker 构造后处于 Idle，Backend 尚未打开；
- Backend 的 open、encode、flush、close 全部发生在 Worker 线程；
- NotEmpty 唤醒、空资源不忙轮询和通知合并；
- 零、单、多及延迟输出保持 metadata 和 AU 顺序；
- FrameStore sequence gap 被统计但 PTS 不被重写；
- AU Queue 满时不继续取输入，NotFull 后先提交 pending；
- `Drain` 排空输入、执行一次 flush 并提交全部输出；
- `Abort` 不 flush，能够在下游不消费时有界完成；
- 启动错误回到 Idle，运行错误只发送一次 Failed；
- stop/restart 重置会话状态且不残留上一会话 metadata。

普通领域测试使用 Fake Backend，不依赖 FFmpeg 或桌面环境。

### 10.2 FFmpeg 集成测试

- 找不到 `libx264` 时给出明确启动错误；
- BGRA 测试图转换为 BT.709 limited-range YUV420P；
- 不同宽高比得到正确黑边颜色与 placement；
- 输入尺寸变化只重建转换状态，输出轨道保持固定；
- 编码输出为可拆分的 Annex-B，IDR 前存在带内 SPS/PPS；
- PTS 有缺口时仍保持单调，AU metadata 与输入匹配；
- flush 不丢失 Backend 已经接受的剩余输出；
- 重复 open/close 和失败清理不泄漏 FFmpeg 资源。

### 10.3 M1 本地闭环

```text
Synthetic 或 DXGI Backend
-> VideoCaptureWorker
-> CapturedVideoFrameStore
-> VideoEncoderWorker
-> EncodedVideoAccessUnitQueue
-> 测试用 H.264 文件消费者
```

测试消费者只用于 M1 验证，不进入正式生产数据流。使用 ffprobe/ffplay 检查可解码性、
1920 x 1080、30 fps、YUV420P、PTS 单调、关键帧间隔与黑边；运行 30 分钟确认内存不持续增长，
并记录预处理、编码、FrameStore 替换、实际码率和采集到编码延迟。正式的可选
`H264FileRecorder` 仍在 VideoRtpSenderWorker 阶段接入。

## 11. 实现顺序

1. 实现并测试 `VideoPlacement`；
2. 定义 `VideoEncoderBackend`、Fake Backend 所需契约和错误类型；
3. 实现 `SwsFrameConverter` 与 `FfmpegH264Encoder` 内部组件；
4. 实现 `FfmpegH264EncoderBackend` 并完成独立 FFmpeg 集成测试；
5. 实现 `VideoEncoderWorker` 的线程、命令、通知、pending 和两种停止路径；
6. 接入已有 FrameStore 与 AU Queue，完成 Synthetic 无设备闭环；
7. 完成真实 DXGI 到 `.h264` 的 M1 验证和性能报告；
8. 性能数据不足时先优化当前 Backend，只有满足拆线程条件后再写新的设计。

## 12. 已决定

- 后端契约只有一个接收 BGRA、输出 H.264 AU 的 `VideoEncoderBackend`；
- 编码契约只依赖共享模型和标准库，不依赖领域模块或 FFmpeg 基础设施；
- 不公开 `VideoFrameProcessor`、通用 YUV 中间对象或 FFmpeg 类型；
- FFmpeg Backend 内部按 placement、swscale 和 codec 职责拆分类与文件；
- 首版使用一个 Video Encode Worker，允许 libx264 内部并行；
- 首版固定 YUV420P、BT.709 limited range、1920 x 1080、30 fps、4 Mbps、GOP 60、无 B 帧；
- 一次输入允许产生零个或多个 AU，normal stop 必须 flush；
- AU Queue 满时保留所有已编码 AU 并停止消费新 BGRA；
- 正常停止使用 Drain，失败和析构清理使用 Abort；
- 输入尺寸可以变化，但输出轨道参数在会话内固定；
- 是否增加独立预处理线程只由可复现性能数据决定。

## 13. 后续阶段再决定

- 运行中动态码率、分辨率或帧率调整；
- 网络反馈触发 IDR、帧率降级和分辨率自适应；
- 硬件编码、GPU 色彩转换与纹理零拷贝；
- 多路编码、Simulcast 或 SVC；
- 性能数据证明必要后的独立 VideoPreprocessWorker 与 YUV 缓冲池。
