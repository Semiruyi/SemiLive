# 视频输出阶段设计

本文定义 Publisher 视频编码输出之后的主输出边界，包括通用 `VideoOutputWorker`、
`VideoAccessUnitOutputBackend` 契约、M1 的 Annex-B 文件实现，以及 M2 的 RTP 实现方式。
视频编码阶段只把完整 `EncodedVideoAccessUnit` 提交到容量受限的 AU Queue，不理解文件、
RTP 或 UDP；输出阶段是该 Queue 的唯一消费者。

## 1. 目标与边界

输出阶段负责：

- 建立独立线程并消费 `EncodedVideoAccessUnitQueue`；
- 把一次发布会话的完整 AU 顺序交给一个主输出 Backend；
- 在 M1 把 Annex-B 字节写入本地 `.h264` 文件；
- 在 M2 把 Annex-B AU 拆分、封装为 RTP 并通过 UDP 发送；
- 对 Queue 空等待、正常排空、故障中止和 Backend 错误提供统一生命周期；
- 记录 AU、字节、输出调用、背压结果和端到端延迟统计。

输出阶段不负责：

- 采集、重复画面、缩放、颜色转换或 H.264 编码；
- 清空 AU Queue；该能力只属于 Controller 持有的 Control 接口；
- 把文件路径、网络地址、Winsock 或 RTP 类型暴露给 `VideoOutputWorker`；
- 在一个 Queue 上注册多个竞争消费者；
- 把 M2 的可选诊断文件记录伪装成第二个主输出。

主输出的最小数据流为：

```text
EncodedVideoAccessUnitQueue
-> VideoOutputWorker
-> VideoAccessUnitOutputBackend
   M1: H264FileOutputBackend
   M2: H264RtpOutputBackend -> DatagramSink
```

每个已启用的视频轨道一次会话恰好选择一个主输出 Backend。M1 的文件输出和 M2 的 RTP 输出
都参与发布正确性：主输出失败会使会话失败，主输出变慢会通过 AU Queue 向编码阶段传播背压。

## 2. 为什么 Worker 与 Backend 分离

`VideoOutputWorker` 是领域编排，稳定职责是线程、命令、资源等待和会话状态。Backend 是可替换
策略，封装具体输出协议和资源。这样文件系统、RTP 和 socket 不进入 Worker，同时 Queue 消费、
Drain/Abort、通知订阅和错误传播只实现一次。

这个抽象只覆盖满足以下条件的主输出：

- 同步接收一个完整视频 AU；
- 调用返回前不再借用 AU；
- 保持输入顺序；
- 输出失败属于当前发布会话的致命错误；
- 可以在 Worker 线程内打开、消费、排空和关闭。

如果未来输出方式不满足这些条件，不扩张本接口去容纳所有行为，而是为它设计独立 Worker 或
异步边界。尤其是 M2 的 best-effort 诊断 Recorder 不属于主输出 Backend。

## 3. Backend 契约

契约位于：

```text
src/publisher/contracts/include/semilive/publisher/contracts/output/
  video_access_unit_output_backend.hpp
```

语义草案如下，具体命名可以在实现时按现有代码风格调整：

```cpp
enum class VideoOutputOperation {
    State,
    Open,
    Consume,
    Flush,
    Close,
};

struct VideoOutputIssue {
    VideoOutputOperation operation = VideoOutputOperation::Open;
    std::int64_t native_code = 0;
    std::string message;
};

struct VideoOutputInfo {
    std::string output_name;
};

struct VideoOutputReceipt {
    std::uint64_t emitted_units = 0;
    std::uint64_t emitted_bytes = 0;
};

class VideoAccessUnitOutputBackend {
public:
    virtual ~VideoAccessUnitOutputBackend() = default;

    [[nodiscard]] virtual std::expected<VideoOutputInfo, VideoOutputIssue>
    open() = 0;

    [[nodiscard]] virtual std::expected<VideoOutputReceipt, VideoOutputIssue>
    consume(const model::EncodedVideoAccessUnit& access_unit) = 0;

    [[nodiscard]] virtual std::expected<VideoOutputReceipt, VideoOutputIssue>
    flush() = 0;

    virtual void close() noexcept = 0;
};
```

约束：

- Backend 的具体配置由 Composition 在构造具体实现时注入，不为文件路径和 RTP endpoint 定义
  通用 variant 配置；
- `open()`、所有 `consume()`、`flush()` 和 `close()` 都在同一个 Output Worker 线程调用；
- `consume()` 同步完成本 AU 的主输出，不保存 AU 引用，也不修改 AU；
- 成功返回表示该 AU 已被 Backend 接受并且不会因调用方释放 AU 而失效；
- `VideoOutputReceipt` 表示本次调用产生的下游单元和字节。文件 Backend 写一次 AU 时通常是
  一个单元；RTP Backend 可以报告多个数据报；
- `flush()` 每个成功打开的会话调用一次。文件实现刷新用户态缓冲，RTP 实现首版可以返回空
  receipt；
- `close()` 幂等且不抛异常，可以清理部分打开、运行失败或未 flush 的会话；
- `consume()` 或 `flush()` 失败后只允许 `close()`，新会话必须重新 `open()`；
- Backend 不创建消费线程，不订阅 AU Queue，也不调用 Controller。

Backend 在构造时持有配置意味着一次 Composition 对象图使用固定输出配置。需要改变文件路径、
目标地址或 Payload 参数时，重新装配对象图；首版不支持运行中切换输出。

## 4. VideoOutputWorker

### 4.1 接口与状态

领域接口位于 `domain/worker/video_output_worker/`。Worker 构造时接收并保存：

```text
unique_ptr<VideoAccessUnitOutputBackend>
EncodedVideoAccessUnitSource&
shared_ptr<Notifier>
```

Worker 构造时建立常驻 `std::jthread`，但不打开 Backend。一次发布会话状态为：

```text
Idle -> Starting -> Running -> Draining -> Idle
  ^        |          |            |
  |        |          +-> Failed --+
  +--------+
   启动失败
```

公开控制语义与 Encoder Worker 保持一致：

```cpp
enum class VideoOutputStopMode {
    Drain,
    Abort,
};

class VideoOutputWorker {
public:
    [[nodiscard]] virtual std::expected<VideoOutputStarted,
                                                VideoOutputWorkerIssue>
    start() = 0;

    virtual void stop(VideoOutputStopMode mode) = 0;

    [[nodiscard]] virtual VideoOutputWorkerState state() const noexcept = 0;
    [[nodiscard]] virtual VideoOutputWorkerStats stats() const noexcept = 0;
};
```

`start()` 在 Worker 线程打开 Backend，成功后返回 `VideoOutputInfo` 并进入 `Running`。输出特有
配置已由 Composition 注入具体 Backend，因此 Worker 的启动接口不接收文件/RTP variant。

### 4.2 运行循环

Worker 订阅 `EncodedVideoAccessUnitQueueNotEmpty`。通知回调只设置 hint 并唤醒自己的条件变量，
不能在 Encoder 或 Controller 线程调用 Backend。每次唤醒后重新查询 Queue 的真实状态。

处理优先级为：

```text
1. 永久关闭或控制命令
2. 从 AU Queue 取出一个 AU
3. 同步调用 Backend.consume(AU)
4. 更新统计并继续查询 Queue
5. Queue 为空时等待 NotEmpty 或控制命令
```

Queue 只在成功 `try_pop()` 后转移 AU 所有权。Backend 调用期间 Worker 保持该 AU 存活，调用
结束后才释放。Backend 失败时该 AU 已离开 Queue，不尝试重新插回队首；会话进入 `Failed`，
Controller 使用故障停止路径清理整条链路。

### 4.3 Drain 与 Abort

`Drain` 只在 Controller 已经确认 Encoder 回到 `Idle` 后调用。此时不再有新 AU 进入 Queue：

```text
Running -> Draining
-> 消费 Queue 中所有 AU
-> Backend.flush()
-> Backend.close()
-> Idle
```

Drain 必须作为运行循环状态推进，不能在命令处理函数中阻塞等待 Queue；这样析构或后续 Abort
仍可唤醒 Worker。Queue 为空且 Backend flush 成功才完成同步 `stop(Drain)`。

`Abort` 立即停止取新 AU，不调用 `flush()`，释放当前内部 AU，调用 `close()` 并回到 `Idle`。
Controller 随后通过 Queue Control 接口清理残留 AU。析构使用独立的永久关闭路径，语义等同
安全 Abort。

同步 Backend 调用本身可能受文件系统或网络 API 延迟影响；首版不通过额外线程伪造硬超时。
M1 文件输出用于本地发布验证，写入介质应为正常本地磁盘。M2 UDP Sink 的阻塞和错误策略在
RTP 设计中明确。

## 5. 背压与所有权

`VideoOutputWorker` 不需要维护额外输入队列。输出慢时，既有有界资源形成背压：

```text
Output Backend 消费变慢
-> EncodedVideoAccessUnitQueue 填满
-> VideoEncoderWorker 保留 pending AU 并停止消费 BGRA
-> CapturedVideoFrameStore 替换最旧未编码帧
-> 内存保持有界，直播画面保持接近实时
```

已编码 AU 不得丢弃，因为后续 P 帧可能依赖它。正常会话必须排空；只有 Abort 或致命错误允许
Controller 清理残留 AU。Output Worker 不持有 Queue Control 接口，从编译边界上禁止自行 clear。

## 6. M1 H264FileOutputBackend

文件 Backend 位于 `infrastructure/output`，构造时接收输出路径。它负责：

- `open()` 以二进制截断模式打开目标文件，并返回最终路径作为输出名称；
- `consume()` 按 AU 顺序写入完整 `annex_b` 字节，不添加长度、时间戳或私有头；
- 拒绝空 AU，并把打开、写入、刷新错误映射为结构化 `VideoOutputIssue`；
- `flush()` 刷新流并验证状态；
- `close()` 幂等关闭文件，不抛异常；
- 不创建目录，不覆盖路径配置语义，不解析 H.264 NAL。

M1 文件是发布会话的主输出，不是诊断旁路。因此文件无法打开或写入失败会使整个发布会话
失败，磁盘写入变慢也允许通过 AU Queue 产生背压。这样正常启动 `semilive_publisher` 就能完成：

```text
DXGI -> Capture Worker -> Encoder Worker -> Output Worker
-> H264FileOutputBackend -> .h264
```

原始 Annex-B `.h264` 不保存每个 AU 的应用层 PTS。Output Worker 在消费时仍检查并统计 PTS 与
source sequence 单调性；生成文件通过 `ffprobe` 和 `ffplay -framerate 30` 做外部可解码性验证。

## 7. M2 H264RtpOutputBackend

M2 保留同一个 `VideoOutputWorker` 和 AU Queue，只在 Composition 中替换主输出 Backend。
`H264RtpOutputBackend` 组合：

```text
H264NalSplitter
-> H264RtpPacketizer
-> DatagramSink
```

它负责 RTP 轨道状态、90 kHz 时间戳映射、序列号、SSRC、Marker、Single NAL、FU-A 和发送
receipt；`DatagramSink` 仍只理解完整数据报，不理解 H.264。Packetizer 保持可独立单元测试。

如果实现过程中发现 RTP 需要异步发送、RTCP 控制、重传或与同步 AU 消费明显不同的生命周期，
不向通用 Backend 强塞这些行为；届时重新评估是否恢复独立 `VideoRtpSenderWorker`。首版无 RTCP、
重传和拥塞控制，满足当前 Backend 边界。

## 8. M2 可选诊断 Recorder

M2 同时启用 RTP 和 `.h264` 诊断记录时，不能让两个消费者从同一个 AU Queue 竞争。RTP 主输出
Backend 在处理 AU 时向独立 `H264FileRecorder` 提交一份 Annex-B 数据副本：

```text
AU Queue -> VideoOutputWorker -> H264RtpOutputBackend -> UDP
                                  |
                                  +-> best-effort diagnostic copy
                                      -> bounded recorder queue -> file
```

Recorder 的提交必须非阻塞，过载或写入失败只停止本次诊断记录，不反压或终止 RTP 发布。它是
独立基础设施能力，不实现 `VideoAccessUnitOutputBackend`，也不成为第二个领域 AU 消费者。

## 9. Composition 与会话顺序

`PublisherConfig` 首版选择文件主输出，Composition 构造 `H264FileOutputBackend` 并注入
`DefaultVideoOutputWorker`。M2 配置选择 RTP 时，Composition 改为构造 RTP Backend；一次轨道
不能同时选择两个主输出。

消费者到生产者的启动顺序：

```text
1. VideoOutputWorker
2. VideoEncoderWorker
3. VideoCaptureWorker
```

正常停止顺序：

```text
1. VideoCaptureWorker.stop()
2. VideoEncoderWorker.stop(Drain)
3. VideoOutputWorker.stop(Drain)
4. Controller 防御性确认资源为空并 clear
```

任一启动或运行阶段发生致命错误时：

```text
1. 停止 Capture
2. Encoder Abort
3. Output Abort
4. clear FrameStore 和 AU Queue
5. 会话进入 Failed
```

Output 必须先于 Encoder 启动并晚于 Encoder 停止，避免 Encoder 因无人消费的满 Queue 在正常
Drain 中等待。M1 的 Ctrl+C 使用正常 Drain，保证已编码且已进入链路的数据写完后再关闭文件。

## 10. 错误与统计

Worker 把 Backend 启动错误同步返回；运行期首次错误进入 `Failed` 并通过 Notifier 发送一次
`VideoOutputWorkerFailed`。错误保留 Worker operation 和可选 Backend issue，不通过日志文本
替代结构化状态。

首版 `VideoOutputWorkerStats` 至少包含：

- Backend 报告的输出名称；
- 消费 AU 数量、关键帧数量和输入 Annex-B 字节数；
- PTS/sequence 非单调错误；
- Backend 调用次数、累计和最大调用耗时；
- Backend receipt 的输出单元数和输出字节数；
- 编码 AU 的 `captured_at` 到输出完成的累计和最大延迟；
- Drain 消费量、Abort 放弃的内部 AU 数量；
- fatal failure 数量和最后一个结构化 Issue。

文件 Backend 不为每个 AU 写日志。会话启动、停止、首个错误和周期聚合统计才进入日志。

## 11. Target 与依赖方向

新增目标建议为：

```text
PublisherContracts
  video_access_unit_output_backend.hpp

PublisherDomain
  VideoOutputWorker / DefaultVideoOutputWorker
  M2 H264RtpOutputBackend + H264NalSplitter + H264RtpPacketizer

PublisherInfraOutput
  H264FileOutputBackend

PublisherInfraTransport
  M2 UdpDatagramSink / MemoryDatagramSink

PublisherInfrastructure (INTERFACE aggregate)
  + PublisherInfraOutput
```

依赖保持：

```text
PublisherDomain -> PublisherContracts + PublisherModel
PublisherInfraOutput -> PublisherContracts + PublisherModel
PublisherInfraTransport -> PublisherContracts
Composition -> PublisherDomain + PublisherInfraOutput + PublisherInfraTransport
```

文件 Backend 不依赖 Domain，Output Worker 不依赖 Infra。测试分别链接精确 target，不能通过
`PublisherCore` 或 `PublisherInfrastructure` 聚合目标获得意外 include 可见性。M2 的
`H264RtpOutputBackend` 与 Packetizer 属于 Domain：它只依赖两个契约接口，不包含具体 socket；
Composition 把 Infra 的 `DatagramSink` 实现注入它，因此不产生 Infra 反向依赖 Domain。

## 12. 测试与验收

### 12.1 Contract 与 Worker 单元测试

- Backend 生命周期顺序和重复调用错误；
- Worker 在同一专用线程调用 open/consume/flush/close；
- Queue NotEmpty 唤醒，空 Queue 不忙轮询；
- AU 顺序、PTS、sequence 和 key-frame metadata 保持；
- Drain 排空 Queue、flush 一次并关闭；
- Abort 不 flush，能够在 Queue 有残留时有界完成；
- Backend 启动失败回到 Idle；
- 运行失败通知一次并要求 stop 后才能重启；
- 析构安全关闭运行中的 Backend。

### 12.2 File Backend 测试

- 二进制截断创建文件；
- 多个 AU 的 Annex-B 字节严格顺序拼接；
- 空 AU、无效路径、写入和 flush 错误得到正确 operation；
- close 幂等，close 后允许重新 open；
- Backend 不保存 AU 引用。

### 12.3 应用闭环

正常运行 `semilive_publisher`，默认以 DXGI、libx264 和文件主输出装配。Ctrl+C 后验证：

- Worker 均按顺序回到 Idle；
- FrameStore 和 AU Queue 为空；
- 文件非空且统计中的输出字节数与文件大小一致；
- PTS 和 source sequence 在应用内严格递增；
- `ffprobe` 识别 H.264、1920x1080、YUV420P；
- `ffplay -framerate 30` 可以连续播放，画面比例、鼠标和关键帧恢复正常。

DXGI 和播放器验证是 Windows 手动验收，不注册为普通无设备 CTest。Synthetic + FFmpeg +
File Backend 的完整链路继续作为自动化 integration test。

## 13. 实现顺序

1. 定义并测试 `VideoAccessUnitOutputBackend` 契约；
2. 实现 `DefaultVideoOutputWorker` 及脚本 Backend 单元测试；
3. 新增 `PublisherInfraOutput` 和 `H264FileOutputBackend`；
4. 扩展无设备链路到真实文件 Backend；
5. 实现首版 Composition、Controller 和 Publisher 文件输出配置；
6. 正常启动应用完成 DXGI 到 `.h264` 的 M1 验收；
7. 实现 NAL Splitter、RTP Packetizer、DatagramSink 和 RTP Backend；
8. Composition 从文件主输出切换为 RTP 主输出；
9. 需要同时诊断时再增加独立 best-effort `H264FileRecorder`。

## 14. 已决定

- AU Queue 一次会话只有一个主输出消费者；
- Queue 消费线程和会话控制属于 `VideoOutputWorker`；
- 具体输出行为由 `VideoAccessUnitOutputBackend` 隔离；
- Backend 同步消费一个完整 AU，不保存引用；
- 具体 Backend 配置在 Composition 构造时注入，不进入通用 Worker 配置；
- M1 的 `H264FileOutputBackend` 是正式主输出，失败会终止会话并参与背压；
- M2 复用 Output Worker，主输出替换为 RTP Backend；
- M2 的可选诊断 Recorder 是独立、异步、best-effort 旁路，不实现主输出 Backend；
- 正常停止按 Capture、Encoder Drain、Output Drain 排空；故障停止使用 Abort；
- 如果 RTP 后续不再满足同步 AU Backend 约束，重新拆出专用 Sender Worker，不污染当前契约。
