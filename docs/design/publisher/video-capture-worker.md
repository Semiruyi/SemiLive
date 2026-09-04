# VideoCaptureWorker 设计

本文定义 Publisher 视频采集 Worker 的线程生命周期、控制接口、采集循环、恢复策略和统计边界。
它把 [`FrameScheduler`](frame-scheduler.md)、
[`DesktopCaptureBackend`](desktop-capture-backend.md) 与 `CapturedVideoFrameStore` 连接为视频链路的
第一个可运行阶段，是 [Publisher 音视频设计总览](overview.md) 中 Video Capture Thread 的详细
设计。

## 1. 目标与边界

`VideoCaptureWorker` 负责：

- 拥有一个与 Worker 对象同寿命的常驻线程；
- 保证 Desktop Capture Backend 只在该线程中打开、调用和关闭；
- 为每次发布会话创建独立的 `FrameScheduler`；
- 可停止地等待视频调度 deadline；
- 缓存最近有效画面的共享不可变引用，并在桌面无变化或短暂不可用时生成重复帧；
- 为实际发布的帧分配连续 `sequence`、计划 PTS 和采集完成时间；
- 非阻塞地把帧发布到 `CapturedVideoFrameSink`；
- 限制后端连续暂时不可用的持续时间；
- 向 Controller 确认启动和停止，并上报第一个运行期致命错误；
- 提供线程安全的状态和统计快照。

`VideoCaptureWorker` 不负责：

- 选择整个发布会话何时开始或编排其他 Worker；
- 缩放、填充黑边、转换 YUV、编码或 RTP 发送；
- 清理 `CapturedVideoFrameStore` 中已经发布的帧；
- 阻塞等待下游腾出空间；
- 在运行中切换桌面输出、帧率或会话时间轴；
- 单独暂停和恢复视频轨道。

Main 不直接访问 Worker。`PublisherComposition` 创建并持有 Worker，`PublisherController` 是其
唯一控制方。Backend 的所有权转移给 Worker 并由 Worker 销毁；FrameStore 和 Notifier 必须比
Worker 活得更久。Worker 通过非拥有的 `CapturedVideoFrameSink` 发布数据。

## 2. 两层生命周期

Worker 对象与一次发布会话具有不同的生命周期。

### 2.1 模块生命周期

构造函数保存长期依赖并启动一个 `std::jthread`。构造函数返回时，线程已经完成入口初始化并
进入 `Idle`，但尚未打开采集后端，也不会产生视频帧。

构造函数等待线程入口确认。若线程创建或 COM 等必要的线程入口初始化失败，构造失败且不向
Composition 暴露部分可用对象；与一次采集会话相关的 Backend 初始化仍放在 `Starting`，通过
`start()` 返回结构化错误。

析构函数用于永久关闭模块：

```text
request shutdown
-> 唤醒内部线程
-> 必要时停止当前会话
-> 在内部线程 close backend
-> 退出线程入口
-> join
```

`std::jthread` 的 stop request 只表达永久退出线程，不用于停止一次发布会话。析构是装配失败和
进程退出的安全兜底，不能替代 Controller 按 Capture、Encoder、Sender 顺序执行的正常停机。

### 2.2 会话生命周期

同一个常驻线程可以顺序执行多次发布会话，但同一时刻最多有一个会话：

```text
Idle -> Starting -> Running -> Stopping -> Idle
  ^        |          |           ^
  |        |          +-> Failed -+
  +--------+
   启动失败
```

启动失败通过 `start()` 返回并回到 `Idle`。只有进入 `Running` 后发生的致命错误进入
`Failed`；此时 Worker 已经停止生产，但 Controller 仍须调用 `stop()` 完成统一的会话清理并
回到 `Idle`，之后才能开始新会话。新的启动会重置会话统计。

析构请求可以从任意状态进入内部 `ShuttingDown`，该状态不需要成为 Controller 可见的会话状态。

## 3. 配置与控制接口草案

### 3.1 会话配置

```cpp
struct VideoCaptureSessionConfig {
    DesktopCaptureConfig capture;
    SessionTimeline timeline;
    FrameRate frame_rate{30, 1};
    std::chrono::milliseconds recovery_timeout{5000};
};
```

Controller 为所有启用的音视频轨道传入同一个 `SessionTimeline`。`track_start` 不属于外部配置：
Worker 在线程内成功执行 `backend.open()` 后记录实际 `steady_clock::now()`，再用它创建
`FrameScheduler`。不同后端初始化耗时造成的轨道起点差异因此保留在共享媒体时间中，而不是由
Controller 猜测或伪装成同时开始。

`recovery_timeout` 必须大于零，首版默认 5 秒。它只限制一次连续的暂时不可用区间；不同恢复
区间不会累计。测试可以配置更短的正值，避免等待 5 秒。

桌面输出、指针合成、帧率、时间轴和恢复上限在一次会话运行期间不可修改。需要修改时停止并
开始新会话，不提供公开 `configure()` / `unconfigure()`。

### 3.2 启动确认

```cpp
enum class VideoCaptureWorkerState {
    Idle,
    Starting,
    Running,
    Stopping,
    Failed,
};

struct VideoCaptureStarted {
    DesktopCaptureInfo source;
    std::chrono::steady_clock::time_point track_start;
    MediaTime first_presentation_time;
};
```

`VideoCaptureStarted` 让 Controller 和诊断输出知道实际选择的桌面、初始尺寸及视频轨道相对会话
原点的真实开始位置。它不承诺第一张桌面图像已经取得或第一帧已经发布。

### 3.3 错误

```cpp
enum class VideoCaptureWorkerOperation {
    Control,
    ThreadInitialization,
    Configure,
    OpenBackend,
    Schedule,
    Capture,
    Recovery,
    Publish,
    Internal,
};

struct VideoCaptureWorkerIssue {
    VideoCaptureWorkerOperation operation;
    std::optional<DesktopCaptureIssue> capture_issue;
    std::string message;
};
```

Backend 错误保留原始 `DesktopCaptureIssue`，包括具体操作、原生错误码和消息。配置错误、调度
溢出、标准异常和未知线程异常由 Worker 转换为对应操作的结构化错误；异常不得越过线程入口。

### 3.4 Worker API

```cpp
class VideoCaptureWorker final {
public:
    VideoCaptureWorker(
        std::unique_ptr<DesktopCaptureBackend> backend,
        CapturedVideoFrameSink& sink,
        std::shared_ptr<Notifier> notifier);
    ~VideoCaptureWorker();

    VideoCaptureWorker(const VideoCaptureWorker&) = delete;
    VideoCaptureWorker& operator=(const VideoCaptureWorker&) = delete;
    VideoCaptureWorker(VideoCaptureWorker&&) = delete;
    VideoCaptureWorker& operator=(VideoCaptureWorker&&) = delete;

    [[nodiscard]] std::expected<VideoCaptureStarted, VideoCaptureWorkerIssue>
    start(VideoCaptureSessionConfig config);

    void stop();

    [[nodiscard]] VideoCaptureWorkerState state() const noexcept;
    [[nodiscard]] VideoCaptureWorkerStats stats() const noexcept;
};
```

这是语义草案，不要求实现逐字照搬。控制接口约束如下：

- `start()` 同步等待内部线程完成 Backend 打开和 Scheduler 创建，返回真实启动结果；
- `start()` 只接受 `Idle`，`Starting`、`Running`、`Stopping` 或 `Failed` 返回控制错误；
- `stop()` 同步等待内部线程关闭 Backend 且确认不会再调用 Sink；
- `stop()` 对 `Idle` 幂等，并把已经停止生产的 `Failed` 会话清理回 `Idle`；
- `state()` 和 `stats()` 返回线程安全快照，但不作为命令完成的同步边界；
- 首版只有 Controller 串行调用控制接口，不把它设计成多调用方并发 API。

`stop()` 的接口草案没有标注 `noexcept`：构造 promise 共享状态和向命令队列插入元素都可能分配
内存。析构使用独立、不分配内存的模块退出路径，不能通过调用公开 `stop()` 实现兜底清理。

### 3.5 内部命令通道

公开接口同步，但所有会话操作仍由 Worker 线程执行。调用线程创建 typed command 和 promise，
将命令放入 Worker 私有队列并唤醒条件变量，然后在本次调用栈中等待 future：

```cpp
using VideoCaptureStartResult =
    std::expected<VideoCaptureStarted, VideoCaptureWorkerIssue>;

struct StartCommand {
    VideoCaptureSessionConfig config;
    std::promise<VideoCaptureStartResult> completion;
};

struct StopCommand {
    std::promise<void> completion;
};

using ControlCommand = std::variant<StartCommand, StopCommand>;
```

概念实现：

```cpp
VideoCaptureStartResult start(VideoCaptureSessionConfig config) {
    StartCommand command{.config = std::move(config)};
    auto completion = command.completion.get_future();
    enqueue(std::move(command));
    return completion.get();
}

void stop() {
    StopCommand command;
    auto completion = command.completion.get_future();
    enqueue(std::move(command));
    completion.wait();
}
```

命令按 FIFO 顺序处理，Worker 循环优先处理控制命令，再处理到期的采集调度点。命令出队后在
控制互斥量之外调用 Backend；完成状态迁移后设置对应 promise。顶层异常处理必须让当前命令的
promise 进入终态，不能让 Controller 永久等待。

这是具体 Worker 的有限命令集合，不引入可扩展的通用 Command Bus。future 只作为接口内部的
跨线程完成机制，不暴露给 Controller。若未来性能或严格的多轨道同时控制确实需要异步接口，
可以让 `start()` / `stop()` 返回已有 completion future，而不改变 Worker 的内部执行模型。

## 4. Controller 使用方式

### 4.1 装配

`PublisherComposition::assemble()` 创建 Backend、FrameStore、Notifier 和 Worker。构造 Worker
会启动常驻线程，但 DXGI 资源直到一次发布会话启动时才创建。

### 4.2 开始发布

Controller 建立共享 `SessionTimeline`，按消费者到生产者顺序同步启动各阶段：

```cpp
auto video_started = video_capture.start(video_config);
if (!video_started) {
    // 回滚已经启动的下游模块
}

auto audio_started = audio_capture.start(audio_config);
if (!audio_started) {
    video_capture.stop();
    // 继续逆序回滚下游模块
}
```

首版接受同阶段 Worker 的有界顺序启动。两条轨道各自在 Backend 成功打开后记录真实
`track_start`，顺序初始化造成的差异由共享媒体时间如实表达，不会伪造为同时产生首个媒体
单元。首轮实现只有视频轨道，不为尚未出现的严格并行启动需求提前暴露异步接口。

若任一 Worker 启动失败，Controller 按逆序同步停止已经启动的同阶段和下游模块。Worker 常驻
线程保留，资源队列由 Controller 的 Control 接口清理。

### 4.3 正常停止

```cpp
video_capture.stop();
audio_capture.stop();
```

首版按确定顺序同步停止 Capture Worker。Backend 查询非阻塞且 deadline 等待可被命令唤醒，因此
每次停止都是有界操作。所有 Capture Worker 停止后，Controller 才命令 Encoder 排空输入并
flush，随后让 Sender 排空编码输出。

`stop()` 返回后保证本 Worker 不会再调用 `CapturedVideoFrameSink::try_push()`；它不保证
FrameStore 已空，也不负责清理 Store。

### 4.4 运行期失败

```cpp
struct VideoCaptureWorkerFailed {
    VideoCaptureWorkerIssue issue;
};
```

进入 `Running` 后发生的第一个致命错误通过 Notifier 上报。Worker 先停止生产、在线程内关闭
Backend、保存失败状态并唤醒等待者，再发送失败通知。Notifier 回调在 Worker 线程同步执行，
因此 Controller 的订阅回调只能保存失败 hint 并唤醒自己的控制等待，不能直接调用
同步 `stop()` 或执行全链路阻塞停止。

首版不引入会话 Generation。Notifier 同步分发，Controller 在开始新会话前已经等待全部 Worker
停止并清理上一轮 hint，因此不存在上一会话的异步通知晚到新会话的问题。

启动期间的错误由 `start()` 返回，不再额外发送运行期失败事件，避免 Controller 对同一
错误执行两次回滚。每次会话只保存和上报第一个致命错误。

## 5. 线程所有权与等待

Worker 线程独占：

- `DesktopCaptureBackend` 的 `open()`、`capture_latest()` 和 `close()`；
- 当前会话的 `FrameScheduler`；
- 最近有效的共享不可变 `BgraFrameBuffer`；
- 下一个发布 `sequence`；
- 连续恢复区间状态；
- 会话内可变统计原值。

需要 COM 的平台在线程入口初始化对应 Apartment，线程退出前先关闭 Backend，再释放 COM。
Backend 不跨线程调用，`FrameScheduler` 也不需要内部同步。

等待 deadline 使用 Worker 自己的条件变量和绝对 `wait_until()`。以下任一情况都必须唤醒：

- 当前 tick 的 deadline 到达；
- StopCommand 入队；
- 模块退出请求。

伪唤醒后重新检查真实谓词和绝对 deadline，不使用反复 `sleep_for()`。FrameStore 的资源通知与
Capture Worker 无关；向 Store 发布始终是非阻塞操作。

会话停止使用 `StopCommand`，不调用 `std::jthread::request_stop()`。后者一旦请求就只用于结束
常驻线程，不能 reset 后服务下一次会话。

## 6. 启动过程

内部线程处理一次启动请求：

```text
1. 校验会话配置
2. 清空最近画面、序号、恢复状态和会话统计
3. backend.open(capture_config)
4. 成功后记录 track_start = steady_clock::now()
5. 创建 FrameScheduler(timeline, frame_rate, track_start)
6. 取得 initial_tick
7. 状态改为 Running，保存 VideoCaptureStarted
8. 完成 StartCommand promise，使 start() 返回
9. 进入采集循环
```

必须先成功打开 Backend，再记录 `track_start`。否则后端初始化耗时会让第一批调度点在 Worker
尚未准备好时过期。若配置校验、Backend 打开或 Scheduler 创建失败，Worker 防御性调用
`close()`，通过 StartCommand promise 返回结构化错误并回到 `Idle`。启动失败不发送运行期失败
通知。

StartCommand 和 StopCommand 在同一 Worker 线程按 FIFO 串行执行；首版 Controller 不并发调用
二者。Backend 的 `open()` 必须是有界操作，因为一个正在执行的 StartCommand 不会被后续命令
抢占。

## 7. 采集循环

一次调度点的处理顺序为：

```text
等待 tick.deadline 或控制命令
-> 有命令则优先处理
-> capture_latest()
-> 处理新图像 / 无变化 / 暂时不可用 / 致命错误
-> 检查是否已有控制命令等待
-> 有缓存时构造并 try_push 当前帧
-> advance_after(steady_clock::now())
-> 累加 skipped_ticks
```

StopCommand 可能在 `capture_latest()` 执行期间入队。Backend 查询是非阻塞、有界操作；调用
返回后 Worker 若发现控制命令等待，则先处理命令而不再发布本次观察。已经进入 `try_push()` 的
调用允许完成，`stop()` 返回负责提供“不再发布”的最终同步边界。

### 7.1 后端结果

| 结果 | 缓存处理 | 当前调度点 |
|---|---|---|
| `DesktopImage` | 像素存储移入新共享缓冲并替换最近画面引用 | 从新缓存构造帧 |
| `DesktopNoChange` | 保持缓存 | 有缓存则构造重复帧，否则不输出 |
| `DesktopTemporarilyUnavailable` | 保持缓存并进入或继续恢复区间 | 未超时且有缓存则构造重复帧 |
| fatal error | 丢弃会话状态并关闭 Backend | 不输出，进入 `Failed` |

`DesktopImage` 或 `DesktopNoChange` 都表示后端当前可用，结束连续恢复区间。尺寸变化的新图像直接
替换旧缓存；固定输出尺寸由后续 [`VideoEncoderBackend`](video-encoding.md) 内部的图像预处理负责。

尚未取得第一张有效图像时不生成黑帧。当前 tick 仍被消耗并计入 `missing_initial_frames`，随后
正常推进 Scheduler，避免取得首图后突发补帧。

### 7.2 画面所有权

Backend 每次取得新桌面状态时返回独占的 `DesktopImage` 值对象。Worker 不把完整图像复制进
缓存，而是将它的 vector 存储移动到共享不可变缓冲：

```cpp
struct BgraFrameBuffer {
    std::vector<std::byte> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
};

using SharedBgraFrameBuffer =
    std::shared_ptr<const BgraFrameBuffer>;
```

转换过程只移动 `std::vector`，不复制它拥有的像素存储。Worker 不保留可变引用，缓存和所有
`CapturedVideoFrame` 只持有 `shared_ptr<const BgraFrameBuffer>`：

```text
DesktopImage
    -> move pixels into immutable BgraFrameBuffer
        ├── Worker latest_image
        ├── CapturedVideoFrame N
        └── CapturedVideoFrame N+1（重复帧时）
```

新桌面图像到达时，Worker 把 `latest_image` 切换到新缓冲；旧缓冲由仍在 FrameStore 或编码线程
中的帧继续持有，最后一个引用释放时自动销毁。桌面无变化时，新帧只复制共享引用和 metadata，
不复制整张图像。

频繁变化的画面仍会为每张真正的新图像分配像素存储，这是 Backend 取得 CPU 快照所必需的；
Worker 不再为缓存额外复制一次。静态画面的多个重复帧共享同一份像素存储。引用计数开销相对
整帧复制很小，首版不再增加对象池、可写复用或 GPU 纹理跨线程所有权。

### 7.3 帧字段

每个实际发布帧满足：

```text
frame.image             = 最近画面的共享不可变引用
frame.sequence          = next_sequence
frame.presentation_time = tick.presentation_time
frame.captured_at       = 本次图像引用准备完成后的 steady_clock::now()
```

`frame.image` 必须非空，尺寸和 stride 与像素存储作为一个不可变对象保持一致。`captured_at` 在
新图像完成所有权转换或重复帧取得缓存引用后、调用 Sink 前记录。它只用于采集后处理延迟统计，
不能替代计划 PTS。`try_push()` 接受帧后才递增 `next_sequence`；Store 替换旧帧仍然算成功发布，
因此序号继续递增。`sequence` 从零连续递增，不等于可能发生跳跃的 `schedule_index`。

## 8. 暂时不可用与致命失败

第一次收到 `DesktopTemporarilyUnavailable` 时记录：

```text
recovering_since = steady_clock::now()
last_recovery_issue = observation.issue
```

后续每个调度点继续调用 Backend，让它执行一次有界重建尝试。只要连续不可用时间小于
`recovery_timeout`，Worker 可以复用最近画面。收到 `DesktopImage` 或 `DesktopNoChange` 后清除
恢复状态，并记录一次恢复成功。

当：

```text
now - recovering_since >= recovery_timeout
```

本次会话以 `Recovery` 错误失败，错误保留最近一次 Backend issue；达到上限的当前 tick 不再
发布陈旧画面。使用持续时间而不是固定失败次数，使策略不依赖帧率，也能正确覆盖 Worker 晚到
跳帧。

下列情况立即成为致命失败，不进入恢复计时：

- Backend 返回 unexpected/error；
- Scheduler 推进发生不可表示的溢出；
- 共享图像缓冲创建、帧构造或 Sink 调用抛出异常；
- Worker 内部状态不变量被破坏。

失败路径必须关闭 Backend、停止生产、进入 `Failed` 并只上报一次。后续 StopCommand 负责把
会话清理回 `Idle`。高频暂时不可用不逐帧写日志；只记录首次进入恢复、恢复成功和最终超时。

## 9. 停止过程

Worker 处理 StopCommand 时：

```text
1. 不再开始新的 capture_latest()
2. 不再调用 CapturedVideoFrameSink::try_push()
3. backend.close()
4. 销毁 FrameScheduler
5. 释放最近桌面画面和恢复状态
6. 状态改为 Idle
7. 完成 StopCommand promise，使 stop() 返回
```

`backend.close()` 幂等且不抛异常。正常停止不清理 FrameStore，因为编码线程需要排空已经接受的
帧；失败停止是否丢弃残留由 Controller 决定。

## 10. 暂停与动态配置

首版不提供 `pause()` / `resume()`。暂停直播需要先统一确定：

- 视频是停止 RTP 还是继续发送重复帧；
- 音频是否同步暂停；
- 暂停时长是否形成媒体时间缺口；
- 恢复时是否请求视频编码器生成 IDR；
- RTP 会话和接收端超时如何维持。

这些是跨轨道、编码和传输的会话语义，应由未来 `PublisherController` 统一设计，不能由 Video
Capture Worker 单独决定。

运行中同样不提供 `configure()`。切换输出、帧率或指针合成通过停止当前会话并使用新配置启动
完成；内部停止过程等价于 unconfigure。

## 11. 统计

`VideoCaptureWorkerStats` 至少提供：

- 已处理调度点数；
- Scheduler 跳过的过期调度点数；
- 新桌面图像数和 NoChange 次数；
- 首图之前未输出的调度点数；
- 发布帧数及其中的重复帧数；
- FrameStore 替换旧帧数；
- 暂时不可用观察次数、恢复区间数和恢复成功数；
- `capture_latest()` 的累计、平均和最大耗时；
- Backend 新图像字节数、源尺寸和尺寸变化次数；
- 致命失败数。

“调度跳帧”“首图缺失”“重复帧”和“Store 替换”分别计数，它们对应不同原因，不能合并成一个
笼统的 dropped frame 指标。统计在每次新会话启动时重置；`stats()` 返回一致的线程安全快照，
不把 Worker 私有计数器暴露给 Controller 修改。

逐帧日志默认关闭。生命周期迁移、第一次进入恢复、恢复成功和最终失败保留低频日志。

## 12. 测试

Worker 测试使用 `SyntheticDesktopCaptureBackend` 或记录调用线程的 Fake Backend，不依赖真实
桌面。异步断言等待明确状态或输出条件，并设置宽松的测试超时；不对操作系统调度的精确毫秒数
作断言，deadline 数学仍由 FrameScheduler 单元测试覆盖。

至少验证：

- 构造启动常驻线程但不打开 Backend，析构能从 Idle 和 Running 安全退出；
- `open/capture_latest/close` 始终发生在同一个 Worker 线程，而不是 Controller 线程；
- 同步 start / stop、内部命令 FIFO 和 stop 幂等语义；
- 启动失败返回结构化错误，运行期失败只发送一次通知；
- stop 命令能立即唤醒远期 deadline 等待，`stop()` 返回后不再向 Sink 发布；
- 首图前 NoChange 不输出，首图后 NoChange 生成内容相同但序号和 PTS 前进的重复帧；
- 新图像的 vector 存储被移动而非整帧复制，重复帧共享同一不可变缓冲；
- 新图像替换缓存，源尺寸变化随下一帧传递；
- 暂时不可用期间复用缓存，恢复后清除计时，连续超时后失败；
- 调度晚到跳帧、首图缺失、重复帧和 Store 替换统计互不混淆；
- PTS 来自 tick，`captured_at` 来自像素准备完成时刻；
- start -> stop -> start 使用新的 Scheduler、序号从零重新开始且不复用旧画面；
- stop 命令与采集或发布相邻时不死锁、不跨会话多发布帧。

可选的 Windows DXGI 集成测试在交互桌面中运行 Worker 数秒，验证真实 Backend 的线程亲和性、
默认指针合成、正常停止和基本帧率；普通 CI 以 Synthetic 链路作为稳定基线。

## 13. 已决定

- Worker 构造时启动常驻线程，析构时永久停止并 join；
- 一次会话的停止与常驻线程的退出使用不同控制信号；
- Backend 的完整生命周期和 FrameScheduler 只在 Worker 线程内操作；
- Controller 使用同步 `start()` / `stop()`，future 只作为内部命令完成机制；
- configure / unconfigure 是 start / stop 的内部步骤，不作为公开接口；
- 首版不支持 pause / resume 或运行中动态配置；
- `track_start` 在 Backend 成功打开后由 Worker 线程记录；
- 首版连续暂时不可用上限为 5 秒，按持续时间而不是失败次数判断；
- 首图前不生成空白帧，之后允许复用最近有效画面；
- Worker 把 Backend 图像移动到共享不可变 BGRA 缓冲，新画面和重复帧均不额外复制整帧；
- 首版不引入像素对象池、可写复用或 GPU 纹理跨线程所有权；
- 正常停止不清理 FrameStore，运行期致命错误通过 Notifier 只上报一次。
