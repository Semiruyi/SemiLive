# SemiLive Receiver 视频链路设计总览

本文定义 SemiLive Receiver 首版视频链路的整体方向，包括职责边界、模块划分、线程模型、
构建依赖、运行时数据流、生命周期和故障恢复原则。具体协议字段、缓存参数、接口签名和算法细节
在对应模块进入实现前再单独确定。

首版 Receiver 与当前 Publisher 配套，只处理单路 H.264/RTP/UDP 视频，并将恢复后的完整
Annex-B Access Unit 交给播放器。视频直连闭环完成后，再进入 Relay 和音频阶段。

## 1. 目标与边界

Receiver 的目标不是掩盖所有网络错误，而是：

> 从不可信 UDP 数据报中恢复结构完整、时间连续性明确的 H.264 AU；不向播放器交付已知损坏的
> AU；连续性破坏后，从下一个完整随机访问点恢复播放。

职责划分：

- Receiver 负责 UDP 接收、RTP 校验、有限重排、丢包确认、H.264 RTP 解包、AU 组装、RTP 时间
  映射以及损坏后的随机访问恢复；
- Receiver 只向下游输出完整 Annex-B AU，不输出单个 RTP 包、FU fragment 或半个 NAL；
- 播放器适配层把 Receiver 的通用 AU 输出契约映射到 SemiPlayer 实时输入 API；
- SemiPlayer 负责 H.264 解码、像素转换、视频调度和画面输出，不解析 UDP/RTP；
- Relay 后续负责会话管理和 RTP 转发，不参与解码，也不改变 Receiver 的输出契约。

首版不实现完整 WebRTC、RTCP 反馈、重传、FEC、拥塞控制、自适应 jitter buffer、多路流和音频。
RTCP、NACK/RTX 与 PLI 的后续范围和实施顺序见
[RTCP 反馈与丢包恢复实施计划](../../plans/weak-network-transport.md)。

## 2. 总体架构

```text
Main
-> ReceiverComposition
   -> ReceiverController
   -> VideoReceiveWorker
      -> DatagramSourceBackend
      -> H264RtpReceivePipeline
         -> RTP parse and session filter
         -> bounded packet reorder
         -> H.264 depacketization
         -> Access Unit assembly
         -> random-access recovery gate
         -> RTP timestamp mapping
      -> LiveVideoOutputBackend
         -> Annex-B file output（阶段验证）
         -> ffplay process output（实时预览）
         -> SemiPlayer adapter（最终输出）
-> ReceiverSessionReport（可选最终 JSON）
```

架构按运行职责分层，并提供不进入媒体热路径的报告模块：

| 层次 | 职责 |
|---|---|
| Model | 定义 UDP 数据报、RTP 包、完整 NAL、实时视频 AU、媒体时间和统计快照等值类型 |
| Contracts | 定义数据报输入与完整视频 AU 输出的后端边界 |
| Domain | 实现 RTP/H.264 处理、恢复状态机、无 I/O 的接收 Pipeline 和 Receive Worker |
| Application | 通过 ReceiverController 管理接收会话，不处理媒体数据 |
| Infrastructure | 实现 UDP Socket、文件验证输出、ffplay 预览和 SemiPlayer 适配器 |
| Composition | 校验配置、选择具体后端、构造对象图并保证逆序释放 |
| Reporting | 在会话结束后把配置与最终统计序列化为机器可读 JSON |

Receiver 不依赖 Publisher namespace。双方协议约定一致，但代码依赖保持独立。只有多个程序长期
共享且边界稳定的类型或算法，才在后续提取到 Common，首版不为复用预先建立通用媒体框架。

## 3. 模块方向

### 3.1 输入后端

`DatagramSourceBackend` 隔离平台网络 I/O。它负责打开监听 endpoint、有界等待 UDP 数据报、
记录单调到达时间、报告平台错误和关闭 Socket，不解析 RTP。

Windows 使用 Winsock 实现，Linux 使用 POSIX Socket 实现。平台类型和错误码不能越过
Infrastructure 边界。

### 3.2 H.264 RTP Receive Pipeline

`H264RtpReceivePipeline` 是 Receiver 的核心无 I/O 状态机，内部保留以下逻辑边界：

- RTP Parser：把一个 UDP 数据报解析为拥有型 RTP 包；
- Session Filter：检查 Payload Type、SSRC 和输入会话一致性；
- Reorder Buffer：在有界时间和容量内恢复小范围乱序，并确认无法补齐的 sequence gap；
- H.264 Depacketizer：恢复 Single NAL 和 FU-A；
- Access Unit Assembler：按 timestamp、sequence 和 Marker 组装完整 AU；
- Timestamp Mapper：扩展 RTP timestamp，并映射到 Receiver 媒体时间；
- Recovery Gate：决定 AU 是否可以交付播放器，以及何时从损坏状态恢复。

这些组件不创建线程、不访问 Socket、不调用 SemiPlayer。Pipeline 可以使用内存数据完成绝大多数
协议、丢包和恢复测试。

### 3.3 输出后端

`LiveVideoOutputBackend` 表示一个实时视频会话的输出边界，负责打开输出、非阻塞接收完整 AU，
以及正常或异常关闭。

输出边界保留三个实现方向：

- Annex-B 文件输出用于先验证 Publisher -> Receiver 的字节和时间边界；
- ffplay 输出通过独立写线程和有界队列把完整 Annex-B AU 交给外部播放器，用于实时预览和弱网
  行为观察；队列饱和时整 AU 返回背压丢弃，不阻塞 Receiver 收包线程；
- SemiPlayer Adapter 用于最终实时播放，内部调用后续增加的 SemiPlayer 实时 AU 输入 API。

Receiver Domain 只依赖输出契约，不包含 SemiPlayer 头文件。SemiPlayer Adapter 只接收完整 H.264
AU 和媒体时间，不把 RTP 概念带入播放器。ffplay 裸 H.264 管道不传递 AU 媒体时间，因此只作为
阶段预览，不承担精确播放调度、画面冻结或端到端延迟测量。

### 3.4 VideoReceiveWorker

`VideoReceiveWorker` 是首版唯一的 Receiver 媒体 Worker。它拥有数据报输入后端、接收 Pipeline、
输出后端和接收工作线程，并按顺序驱动整条链路。

协议解析、重排和组帧对于当前单路 1080p30 视频足够轻量，首版不拆分网络线程和协议线程。
只有性能测量证明单线程处理阻塞 Socket 接收时，才增加新的 Worker 和线程间有界队列。

### 3.5 ReceiverController

Controller 只提供接收会话的启动、停止、状态、等待和统计接口，不访问 UDP 包、NAL 或 AU。
首版只有一个 VideoReceiveWorker，因此 Controller 不创建独立控制线程；公开控制调用同步串行化，
运行期结果由 Worker 的会话状态通知等待者。

后续加入音频时，Controller 再负责协调音视频 Worker 的共同启动、停止和失败策略。

### 3.6 Session Reporting

`ReceiverSessionReport` 不参与收包、重排或输出，也不由 Domain 调用。Main 在 Receiver 停止并取得
不可变统计快照后，按 `--stats-json` 配置写出会话配置、RTP/H.264 计数、输出计数和单调时钟指标。

启动时等待第一个完整 SPS/PPS/IDR 计入 `first_output_delay`，但不计为弱网恢复 episode。只有已经
进入 Streaming 后再次进入 `WaitingForRandomAccess`，才开始一次恢复计时；重复 discontinuity 不
重置开始时间。已完成恢复的等待时间与会话结束时仍在进行的 `active_wait` 分开记录，同时提供
包含 active wait 的总值和最大值。

`maximum_output_gap` 只覆盖相邻成功提交 AU 的间隔；最后一次成功提交到会话结束是右截尾样本，
单独记录为 `terminal_output_gap`，不计入 gap 最大值或 stall。`output_stall` 使用报告中固定的阈值
统计事件数和超出阈值的累计时长。这些字段只作为当前文件或 ffplay 输出阶段的链路停顿代理，
不表述为播放器画面冻结。

## 4. 线程模型

首版线程关系：

```text
Main thread
  -> CLI / Ctrl+C / ReceiverController / statistics

VideoReceiveWorker thread
  -> UDP receive
  -> RTP reorder and H.264 reconstruction
  -> AU recovery gate
  -> non-blocking output submission

SemiPlayer-owned threads
  -> decode / convert / schedule / display callback
```

Receiver 自己只增加一个后台线程。该线程由 `VideoReceiveWorker` 拥有，并只在接收会话期间存在；
重新开始会话时创建新线程和新的协议状态。

Socket、重排状态、FU-A 重组状态、AU 组装状态和时间戳映射状态均由 Receive Worker 线程独占，
不需要在媒体热路径上加锁。Controller 只通过线程安全的生命周期接口控制 Worker。

输出提交必须非阻塞。播放器暂时无法接受 AU 时，Receiver 不等待播放器腾空，而是执行实时视频
丢弃与随机访问恢复策略。

## 5. 运行时数据链路

正常链路：

```text
UDP datagram
-> parsed RTP packet
-> ordered RTP packet
-> complete H.264 NAL
-> complete candidate AU
-> playable AU
-> LiveVideoOutputBackend
-> SemiPlayer decoder pipeline
```

处理原则：

1. UDP Backend 返回拥有型数据报及其到达时间；
2. RTP Parser 校验包结构并提取 sequence、timestamp、SSRC、PT、Marker 和 payload；
3. Reorder Buffer 在有限窗口内恢复乱序，只有超过边界才确认丢包；
4. Depacketizer 只产生完整 NAL，损坏或不完整 FU-A 不进入 AU；
5. Assembler 保持 NAL 顺序，并结合 timestamp 和 Marker 确认 AU 边界；
6. Recovery Gate 过滤启动前或故障后的非随机访问 AU；
7. Timestamp Mapper 将 RTP 时钟转换为单调媒体时间；
8. Worker 将拥有型 Annex-B AU 非阻塞提交给输出后端。

同一个数据报从解析到重排尽量通过移动所有权避免重复复制。首版接受在组装连续 Annex-B AU 和
跨 SemiPlayer ABI 时发生必要复制，不提前引入 Buffer Pool 或跨项目共享内存所有权。

## 6. 丢包与恢复方向

Receiver 不在看到乱序时立即宣布当前 GOP 损坏，而是在有限重排仍无法补齐后确认 sequence gap。

确认连续性破坏后：

```text
sequence gap / damaged FU-A / incomplete AU / output drop
-> discard current NAL and AU
-> enter WaitingForRandomAccess
-> discard following non-IDR AUs
-> accept next complete SPS/PPS/IDR AU
-> mark discontinuity before that AU
-> SemiPlayer resets decoder state
-> resume Streaming
```

“SPS/PPS 到达”本身不是恢复完成。首版要求一个结构完整、包含 SPS、PPS 和 IDR VCL 的随机访问 AU
成功交给播放器后，才重新进入 Streaming。

Receiver 负责识别恢复边界并显式传递 discontinuity；SemiPlayer 不根据普通 IDR 自行猜测此前是否
发生丢包。健康码流中的周期 IDR 不触发无条件 Decoder Reset。

首版采用保守策略：任何已经确认影响 AU 连续性的丢包都等待下一 IDR，不分析 Slice 参考关系来
尝试保留当前 GOP。后续加入 RTCP 反馈时，再设计主动请求关键帧和更快恢复。

## 7. 构建与依赖方向

Receiver 建议建立以下 target 分组：

```text
SemiLive::ReceiverModel
SemiLive::ReceiverContracts
SemiLive::ReceiverDomain
SemiLive::ReceiverApplication
SemiLive::ReceiverInfraNetwork
SemiLive::ReceiverInfraFileOutput
SemiLive::ReceiverInfraFfplayOutput
SemiLive::ReceiverInfraSemiPlayer
SemiLive::ReceiverInfrastructure
SemiLive::ReceiverComposition
SemiLive::ReceiverReporting
SemiLive::ReceiverCore
semilive_receiver
```

固定依赖方向：

```text
ReceiverModel
    ^
ReceiverContracts
    ^
ReceiverDomain
    ^
ReceiverApplication
    ^
ReceiverComposition
    ^
ReceiverReporting
    ^
ReceiverCore
    ^
semilive_receiver
```

Infrastructure 只实现 Contracts：

```text
ReceiverInfraNetwork ---------> ReceiverContracts + ReceiverModel
ReceiverInfraFileOutput ------> ReceiverContracts + ReceiverModel
ReceiverInfraFfplayOutput ----> ReceiverContracts + ReceiverModel
ReceiverInfraSemiPlayer ------> ReceiverContracts + ReceiverModel
                                 + SemiPlayer public SDK

ReceiverComposition ----------> Application + Domain + concrete Infrastructure
ReceiverReporting ------------> Composition + Application statistics
```

约束：

- Domain 不链接 Winsock、FFmpeg、SemiPlayer 或文件输出实现；
- Application 不创建具体 Backend；
- Composition 是唯一同时知道抽象接口和具体实现的模块；
- Main 只链接 `ReceiverCore` 和公共日志；
- 每个 target 只公开自己的 `include` 根，不公开项目 `src` 根；
- 测试只链接被测 target，利用编译和链接边界阻止跨层依赖；
- SemiPlayer 通过公开、版本化的 SDK target 接入，不直接包含兄弟仓库的内部源码。

SemiPlayer 实时输入尚未完成时，Receiver 可以构建文件输出与 ffplay 外部预览链路。正式实时播放
阶段再加入可选的 SemiPlayer SDK 构建依赖，最终发布和 CI 使用固定版本完成可复现构建。

## 8. Composition 与所有权

运行时对象图：

```text
ReceiverComposition
  -> ReceiverController
       --non-owning control access--> VideoReceiveWorker

  -> VideoReceiveWorker
       -> owns DatagramSourceBackend
       -> owns H264RtpReceivePipeline
       -> owns LiveVideoOutputBackend
       -> owns session worker thread
```

Composition 负责：

- 校验进程级配置；
- 根据构建能力和运行配置选择文件或 SemiPlayer 输出；
- 创建具体输入、输出、Pipeline、Worker 和 Controller；
- 向 Main 暴露有效期受限的 Controller 非拥有访问；
- 组装失败时按创建逆序回滚；
- 释放时先停止活动会话，再按依赖逆序销毁对象。

`assemble()` 只建立对象图，不 bind UDP、不打开输出，也不启动接收线程。外部资源在
`start_receiving()` 对应的会话启动阶段获取。

## 9. 程序生命周期

进程主路径：

```text
parse CLI
-> initialize log
-> create ReceiverComposition
-> assemble
-> obtain ReceiverController
-> start_receiving
-> wait for Ctrl+C or terminal failure
-> stop_receiving when needed
-> print final statistics
-> dispose composition
-> shutdown log
```

会话启动遵守消费者先于生产者：

```text
1. 创建会话线程并重置 Pipeline 与统计
2. 打开 LiveVideoOutputBackend
3. 打开并 bind DatagramSourceBackend
4. 进入 WaitingForRandomAccess
5. 确认 Receiver Running
```

UDP 打开失败时关闭已经打开的输出并回滚。输出打开失败时不开始监听 UDP。

正常停止：

```text
1. 停止接收新数据报
2. 丢弃重排缓存、未完成 FU-A 和未完成 AU
3. 关闭 UDP 输入
4. 正常关闭输出后端
5. Worker 线程退出并 join
6. Controller 回到 Idle
```

Receiver 是实时链路，停止时不等待未知的缺失包，也不把半成品排空给播放器。

不可恢复的 Socket、输出或内部资源错误使会话进入 Failed，停止输入并以 Abort 方式关闭输出。
RTP 畸形、丢包、乱序超时和损坏 AU 属于媒体事件，只改变恢复状态和统计，不直接终止进程。

Composition 的 `dispose()` 幂等；如果会话仍活动，先通过 Controller 停止，再销毁 Controller、
Worker 及其拥有的 Backend。Worker 析构必须兜底停止并 join 线程，不能让线程访问已经释放的依赖。

## 10. 验证阶段

Receiver 按以下阶段推进：

1. 纯内存验证 RTP 解析、有限重排、FU-A 重组、AU 边界和 IDR 恢复；
2. 接入 UDP loopback 和 Annex-B 文件输出，验证 Publisher -> Receiver 码流等价性；
3. 接入有界异步 ffplay 输出，形成不绕过 Receiver Pipeline 的实时预览；
4. 改造 SemiPlayer 实时 AU 输入并实现输出 Adapter；
5. 完成 Publisher -> Receiver -> SemiPlayer 直连播放；
6. 注入丢包、乱序和损坏 FU-A，验证只从完整 IDR 恢复；
7. 完成 1080p30、30 分钟稳定性和端到端指标报告；
8. 视频直连闭环稳定后，再设计 Linux Relay。

首版设计优先保证边界清晰、行为有界、故障可解释和闭环可复现，不为尚未实现的音频、反馈或
多路会话提前增加抽象。
