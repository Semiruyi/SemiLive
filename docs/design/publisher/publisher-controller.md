# PublisherController 设计

本文定义 Publisher 应用层的首版会话 Controller。它在 Composition 创建的固定对象图之上编排
一次视频发布会话，不创建媒体对象，也不理解 DXGI、FFmpeg、文件或 RTP 实现。

## 1. 模块与依赖

Controller 位于独立静态库：

```text
src/publisher/application/
  include/semilive/publisher/application/publisher_controller/
    publisher_controller.hpp
    default_publisher_controller.hpp
  src/publisher_controller/
    default_publisher_controller.cpp
```

依赖方向为：

```text
PublisherApplication -> PublisherDomain -> PublisherContracts + PublisherModel
```

`PublisherApplication` 不依赖 `PublisherInfrastructure`。Composition 拥有完整对象图，最后创建
Controller，并保证 Worker、资源和 Notifier 比 Controller 活得更久。Controller 只保存非拥有的
Worker/Control 引用以及一个共享 Notifier 引用。

`DefaultPublisherController` 使用 PImpl，避免把控制线程、同步原语、命令队列和订阅生命周期
暴露到公开头文件。

## 2. 职责

Controller 负责：

- 保证同一时刻只有一个发布会话；
- 每次开始时创建新的不可变 `SessionTimeline`；
- 按消费者到生产者顺序启动 Worker；
- 正常停止时按生产者到消费者顺序 Drain；
- 启动失败时只回滚已经成功启动的阶段；
- 汇聚 Worker 的首个运行期致命错误并执行全链路 Abort；
- 停止后通过资源 Control 接口防御性清理残留；
- 提供状态、限时等待、会话结果和统计快照；
- 完整停止后复用同一对象图开始下一次会话。

Controller 不负责：

- 创建 Backend、Worker、资源或 Notifier；
- 解析命令行、安装 Ctrl+C handler 或初始化日志；
- 通过资源 Sink/Source 接口传递媒体数据；
- 在运行中更换输出路径、Backend 或媒体配置；
- 实现采集、编码、文件、RTP 或 socket 逻辑。

## 3. 配置与构造

进程配置先传给 Composition。Composition 使用输出路径构造具体 Backend，并把每次会话固定使用
的视频设置整理成 `PublisherVideoSessionPlan`：

```cpp
struct PublisherVideoSessionPlan {
    DesktopCaptureConfig capture;
    std::chrono::milliseconds recovery_timeout{5000};
    VideoEncoderConfig encoder;
};
```

采集和编码共用 `encoder.frame_rate`，避免同一个对象图出现两个互相矛盾的帧率配置。输出配置
属于 Composition 构造的具体 Backend，不进入 Controller Plan。

Controller 还接收类型明确的视频管线控制面：

```cpp
struct PublisherVideoPipeline {
    VideoCaptureWorker& capture_worker;
    VideoEncoderWorker& encoder_worker;
    VideoOutputWorker& output_worker;
    CapturedVideoFrameStoreControl& frame_store;
    EncodedVideoAccessUnitQueueControl& access_unit_queue;
};
```

首版不提取通用 Worker、媒体 Graph 或阶段容器。音频实现时按明确类型扩展 Plan 和 Pipeline，
避免提前用运行期类型判断替代编译期依赖。

## 4. 对外接口

Composition 只向 Main 提供 `PublisherController&`。Main 使用：

```text
start_publishing()
stop_publishing()
state()
stats()
wait_for_terminal_for(timeout)
```

`start_publishing()` 成功返回 session id、共享 timeline 和三个 Worker 的启动信息。启动失败返回
带 Controller 阶段和原始 Worker issue 的结构化错误。

`stop_publishing()` 是同步接口：正常返回时所有 Worker 已回到 Idle，Backend 已关闭，两个资源
已完成防御性 clear。Idle 状态调用是幂等成功；Failed 状态调用表示调用方确认失败并把 Controller
恢复为 Idle。

`wait_for_terminal_for()` 供 Main 在安全的普通执行上下文中等待 Idle 或 Failed。超时用于 Main
定期检查 Ctrl+C 标记；操作系统信号回调只设置标记，不能直接调用 Controller。

## 5. 状态机

公开状态保持为：

```text
Idle -> Starting -> Running -> Stopping -> Idle
          |           |           |
          +-> Idle    +-----------+-> Failed
          启动失败       运行或停止失败
```

- 只有 Idle 可以开始新会话；
- 每次开始尝试分配递增 session id；
- 启动失败完成回滚后直接回到 Idle，并同步返回错误；
- 运行期失败先进入 Stopping，完成 Abort 和资源清理后才发布 Failed；
- Failed 保留首个致命错误，直到 `stop_publishing()` 确认；
- Failed 未确认时拒绝新的 start，避免旧故障被静默覆盖。

## 6. 启动与回滚

开始前先 clear 两个资源，然后创建本次会话的 timeline：

```text
Output.start
-> Encoder.start
-> Capture.start
-> Running
```

失败时只回滚已成功启动的阶段：

```text
Output 失败:  clear resources
Encoder 失败: Output Abort -> clear resources
Capture 失败: Encoder Abort -> Output Abort -> clear resources
```

启动返回成功只确认三个 Worker 的 Backend 均已打开；随后立即发生的媒体处理错误仍作为运行期
失败异步汇聚。

## 7. 正常停止与可中断 Drain

正常停止顺序为：

```text
Capture.stop
-> Encoder Drain
-> Output Drain
-> clear resources
-> Idle
```

Worker 的 `stop(Drain)` 是同步接口。Controller 不能直接在自己的控制线程中等待 Encoder Drain，
因为此时 Output 仍可能失败并停止消费；如果 AU Queue 已满，Encoder Drain 会等待 Output，控制
线程又无法处理失败命令，形成环形等待。

因此 `DefaultPublisherController` 把一次同步 Drain 放到短生命周期等待线程中，控制线程继续处理
类型化命令：

```text
Controller control thread -> launch Encoder Drain waiter
                          -> continue processing WorkerFailed

Output failure -> Controller control thread -> Encoder Abort
                                          -> Drain waiter returns
                                          -> abort remaining stages
```

同一时刻最多存在一个 Drain 等待线程。它不处理媒体数据，只把同步 Worker 调用完成结果投递回
Controller 控制线程。正常 Encoder Drain 完成后才启动 Output Drain。

## 8. 运行期失败

Controller 在构造时订阅三个 Worker 的 Failed 事件。Notifier 回调运行在发送事件的 Worker
线程，因此回调只复制首个 issue、附加当前 session id 并唤醒 Controller；它不停止 Worker、
不清理资源，也不等待线程。

Controller 控制线程执行：

```text
Capture.stop
-> Encoder Abort
-> Output Abort
-> join active Drain waiter
-> clear FrameStore and AU Queue
-> Failed
```

同一会话只接受第一个 Worker 失败作为主错误。后续级联错误不覆盖根因，也不重复增加失败会话
计数。事件携带 session id，旧会话命令不能影响新会话。

## 9. 统计

首版 `PublisherControllerStats` 聚合：

- 当前状态和 session id；
- 已启动、正常完成、运行失败和启动失败次数；
- 防御性清理的帧与 AU 总数；
- 两个资源的当前与峰值水位；
- Capture、Encoder 和 Output Worker 快照；
- 最近一个 Controller/Worker 结构化错误。

运行期间各 Worker 快照不承诺同一个纳秒时刻的跨线程事务一致性；会话终止后最终数据稳定。
首版不额外创建共享 `PublisherStats` 服务，多轨统计或外部指标输出出现明确需求后再评估。

## 10. 析构

Composition 正常释放前先显式停止或确认会话。Controller 析构仍执行兜底流程：

```text
disable callbacks
-> unsubscribe
-> stop accepting public commands
-> Abort active pipeline
-> join Drain/control threads
-> clear resources
```

Controller 必须先于 Worker 和资源销毁。析构不抛异常。

## 11. 测试基线

- 消费者到生产者的启动顺序；
- 生产者到消费者的正常 Drain 顺序；
- 三个启动阶段的精确回滚范围；
- 重复 start、Idle stop 和多次会话；
- Worker 运行期失败的异步 Abort 与失败确认；
- 第一个运行错误不被后续错误覆盖；
- Output 在 Encoder Drain 期间失败时能够中断 Drain；
- 失败回调线程不直接执行 Worker stop；
- Controller 驱动 Synthetic、FFmpeg 和 H264File 的真实文件链路；
- Application target 的编译命令不包含 Infrastructure include 路径。
