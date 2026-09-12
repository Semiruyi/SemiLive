# RTP/UDP 视频输出 Backend 设计

本文定义 Publisher M2 的 `RtpUdpVideoOutputBackend`。它实现已有
`VideoAccessUnitOutputBackend` 契约，由 `PublisherComposition` 创建并注入
`DefaultVideoOutputWorker`，把编码器产生的 Annex-B H.264 Access Unit 封装为 RTP 包并通过
UDP 发送。

本文只覆盖 Publisher 的单路 H.264 RTP/UDP 主输出。Receiver、Relay、RTCP、会话描述、反馈与
重传不在本文范围内。通用输出线程、Queue 背压和 Drain/Abort 语义继续由
[视频输出阶段设计](video-output.md)定义。

协议行为以 [RFC 3550](https://www.rfc-editor.org/rfc/rfc3550) 和
[RFC 6184](https://www.rfc-editor.org/rfc/rfc6184) 为准。

## 1. 已选架构

一次视频发布会话仍只有一个主输出消费者：

```text
EncodedVideoAccessUnitQueue
-> DefaultVideoOutputWorker
-> VideoAccessUnitOutputBackend
-> RtpUdpVideoOutputBackend
   -> Annex-B NAL 拆分
   -> H.264 RTP Single NAL / FU-A 封包
   -> UDP Socket
-> Receiver 或 Relay

RtpUdpVideoOutputBackend
   +-> optional H264FileRecorder
       -> bounded debug queue -> .h264 file
```

职责分配如下：

- `DefaultVideoOutputWorker` 负责线程、Queue 消费、Backend 生命周期、Drain/Abort、错误传播和
  通用输出统计；
- `VideoAccessUnitOutputBackend` 保持协议无关，只同步接收完整
  `EncodedVideoAccessUnit`；
- `RtpUdpVideoOutputBackend` 是 infrastructure 实现，负责 Annex-B、H.264/RTP 状态和 UDP
  Socket；
- `PublisherComposition` 始终创建 RTP/UDP 主输出，并把 Backend 注入
  `DefaultVideoOutputWorker`；启用文件调试时，再把可选 `H264FileRecorder` 注入 RTP Backend；
- Main 只解析配置和展示会话结果，不创建 Socket，也不参与 RTP 封包。

首版不增加跨层 `DatagramSink` 契约，也不新增 `VideoRtpSenderWorker`。Packetizer 和 Socket
封装可以拆为 Backend 模块内部的可测试组件，但它们不进入 Domain 或 Contracts。

## 2. 为什么 RTP 与 UDP 都放在 Backend

当前 `VideoAccessUnitOutputBackend::consume()` 的同步语义适合首版 RTP/UDP：调用接收一个完整
AU，按顺序形成若干 RTP 数据报，并在所有数据报交给操作系统后返回。Worker 不需要理解一个 AU
对应多少个包，也不需要管理 sequence、SSRC、MTU 或 Socket。

这样可以保持以下依赖方向：

```text
PublisherDomain -> PublisherContracts + PublisherModel
PublisherInfraOutput -> PublisherContracts + PublisherModel
PublisherComposition -> PublisherDomain + PublisherInfraOutput
```

RTP/UDP 主输出满足现有 Backend 边界：

- 所有方法只由 Output Worker 线程调用；
- `consume()` 返回前不再借用 AU；
- AU 和 NAL 的发送顺序与输入顺序一致；
- 输出失败是当前发布会话的致命错误；
- Backend 不创建发送线程，也不维护无界发送队列。

如果以后引入 RTCP、重传、拥塞控制、paced sending 或跨 AU 异步发送，使 Backend 需要独立线程
和独立生命周期，再设计专用 Sender Worker；首版不为这些未来能力扩张当前接口。

## 3. 模块与文件布局

建议把实现放入现有 `publisher/infrastructure/output` target：

```text
src/publisher/infrastructure/output/
  include/semilive/publisher/infrastructure/output/
    rtp_udp_video_output_backend.hpp
  src/
    rtp_udp_video_output_backend.cpp
    rtp/
      annex_b_nal_splitter.hpp
      annex_b_nal_splitter.cpp
      h264_rtp_packetizer.hpp
      h264_rtp_packetizer.cpp
      rtp_session_state.hpp
    udp/
      udp_socket.hpp
      udp_socket.cpp
```

只有 `RtpUdpVideoOutputBackend` 是模块公开类型。NAL Splitter、Packetizer、RTP 会话状态和 UDP
Socket 都是 implementation detail，头文件不安装到公共 include 根。测试通过对应 target 的私有
测试入口或直接编译纯算法源文件，不把内部类型升级成跨层契约。

实现时允许合并过小文件，但必须保留三条逻辑边界：

```text
Annex-B byte stream -> NAL views
NAL views + RTP state -> RTP datagrams
RTP datagram -> UDP send
```

## 4. Backend 配置

配置属于 Composition，不进入 `VideoOutputWorker`：

```cpp
struct RtpUdpVideoOutputConfig {
    std::string destination_address;
    std::uint16_t destination_port = 0;
    std::uint8_t payload_type = 96;
    std::size_t max_datagram_bytes = 1200;
};
```

字段语义：

- `destination_address`：首版只接受数值形式的 IPv4 或 IPv6 地址，不在 `open()` 中执行可能长时间
  阻塞的 DNS 查询；
- `destination_port`：必须为 `1..65535`；
- `payload_type`：RTP 7-bit PT，首版要求动态范围 `96..127`，默认 `96`；
- `max_datagram_bytes`：一次 UDP payload 的最大字节数，包含 12 字节 RTP Header，不包含 UDP、IP
  和链路层 Header；默认 `1200`，用于避免常见网络路径上的 IP 分片。

`max_datagram_bytes` 必须至少容纳 RTP Header、FU Indicator、FU Header 和一个 NAL payload 字节，
并且不得超过平台允许的 UDP payload 上限。实现使用显式上限校验，不能依赖 `send()` 才发现配置
错误。

SSRC、初始 sequence 和初始 RTP timestamp 不作为普通用户配置。`open()` 每次新会话通过操作系统
随机源生成；测试通过 Backend 内部的确定性状态工厂注入固定值。随机源不可用时 `open()` 失败，
不退化为时间戳、进程号或固定常量。

## 5. Composition、调试记录与 CLI

`PublisherConfig` 明确区分正式主输出和可选调试输出：

```cpp
struct PublisherH264DebugOutputConfig {
    std::filesystem::path path;
};

struct PublisherVideoOutputConfig {
    RtpUdpVideoOutputConfig rtp_udp;
    std::optional<PublisherH264DebugOutputConfig> debug_file;
};

struct PublisherConfig {
    PublisherVideoConfig video;
    PublisherVideoOutputConfig output;
};
```

Composition 始终装配 RTP 主输出：

```text
optional debug_file
-> optional H264FileRecorder

RtpUdpVideoOutputConfig + optional recorder
-> make_unique<RtpUdpVideoOutputBackend>()
-> unique_ptr<VideoAccessUnitOutputBackend>
-> DefaultVideoOutputWorker
```

`H264FileRecorder` 由 `RtpUdpVideoOutputBackend` 独占拥有。Backend 每次消费 AU 时先进行一次
非阻塞调试提交，再继续 RTP 封包和 UDP 发送。文件队列满、文件打开/写入失败或调试功能关闭都
不能改变 RTP 主输出结果；Recorder 失败后禁用本次会话的文件记录并保留非致命诊断信息。

现有 `H264FileOutputBackend` 保留用于 M1 历史验收、Backend 单元测试和隔离编码调试，但目标
Composition 不再把它作为正式主输出注入 Worker。正常 Publisher CLI 使用：

```text
--rtp-address ADDRESS
--rtp-port PORT
--rtp-payload-type PT          默认 96
--rtp-max-datagram-bytes SIZE 默认 1200
--debug-h264 PATH              可选保存 Annex-B 调试文件
```

`--rtp-address` 和 `--rtp-port` 必须同时出现；没有合法 RTP endpoint 时启动失败。
`--debug-h264` 只增加旁路调试记录，不切换、替代或影响 RTP 主输出。当前 M1 的 `--output PATH`
在接入 RTP Composition 时由 `--debug-h264 PATH` 替代；项目尚未发布稳定 CLI，不保留两个含义
不同的长期别名。

## 6. 生命周期与状态

Backend 内部至少区分：

```text
Closed -> Open -> Failed
   ^        |
   +--------+
      close
```

### 6.1 open

`open()` 在 Output Worker 线程依次完成：

1. 确认当前为 `Closed`；
2. 校验 endpoint、PT 和数据报大小；
3. 把数值地址转换为平台 endpoint；
4. 创建与地址族匹配的 UDP Socket，并固定远端 endpoint；
5. 将 Socket 设置为非阻塞，保证发送调用有界；
6. 生成 SSRC、初始 sequence 和初始 timestamp；
7. 启用调试文件时，以 best-effort 打开 `H264FileRecorder`；失败只禁用本次调试记录；
8. 清空上次会话的 scratch buffer 与主输出统计状态；
9. 进入 `Open` 并返回可读的输出名称。

输出名称使用类似 `rtp+udp://127.0.0.1:5004?pt=96` 的诊断字符串；IPv6 地址使用方括号。它只用于
展示，不作为可再次解析的会话描述。

UDP 的“固定远端”不表示建立可靠连接或握手。实现可以使用 connected UDP Socket 简化每包发送，
但不能把 `open()` 成功解释为 Receiver 或 Relay 已在线。

### 6.2 consume

`consume(access_unit)` 必须：

1. 确认状态为 `Open`；
2. 防御性校验 AU 非空、`presentation_time >= 0`；
3. 启用调试文件时，向 Recorder 非阻塞提交 Annex-B 副本；拒绝或失败不改变主流程；
4. 从 `annex_b` 拆出至少一个合法 NAL；
5. 将 AU 的 `presentation_time` 映射为一个 RTP timestamp；
6. 按 NAL 原顺序生成 Single NAL 或 FU-A 包；
7. 为每个 RTP 包分配连续 sequence 并同步发送；
8. 全部发送成功后返回本 AU 的 receipt。

成功返回意味着所有 RTP 数据报已经被本机 UDP 栈接受，不意味着网络送达或接收端解码成功。

任一封包或发送步骤失败后 Backend 进入 `Failed`，本次调用返回
`VideoOutputOperation::Consume`。失败前可能已有部分数据报发出，Backend 不回滚、不重发，也不
继续下一个 AU；Controller 随后按既有故障路径 Abort 整条发布链路。

### 6.3 flush 与 close

首版没有 RTP 私有发送队列；可选 Recorder 的有界调试队列不属于主输出：

- `flush()` 在 `Open` 状态不发送额外 RTP 数据；
- 启用调试文件时，`flush()` 请求 Recorder 排空已经接受的调试 AU；Recorder 的 flush 或写入
  失败只记录非致命诊断，不把 RTP 主输出改为失败；
- 主输出 `flush()` 仍返回空 receipt；
- `flush()` 不生成 RTCP BYE、EOS NAL 或私有结束包；
- `close()` 幂等关闭 Socket 和 Recorder，释放 buffer，并回到 `Closed`；未先 flush 的 Abort
  允许 Recorder 丢弃未写入的调试数据；
- `Failed` 后只允许 `close()`；
- 正常 Drain 仍由 Worker 先消费完 AU Queue，再调用 `flush()` 和 `close()`；
- Abort 不调用 `flush()`，直接 `close()`。

## 7. Annex-B NAL 拆分

`EncodedVideoAccessUnit::annex_b` 是编码 Backend 输出的完整 Annex-B AU。NAL Splitter 返回指向
AU 原始字节的只读 view，不复制 NAL payload。

首版规则：

- 识别三字节 `00 00 01` 和四字节 `00 00 00 01` start code；
- start code 不进入 RTP payload；
- 保持 NAL 在 AU 中的原顺序；
- 接受第一个 start code 前 Annex-B 允许的零字节；
- 排除 NAL 之间和 AU 末尾属于 Annex-B 的 trailing zero bytes；
- 拒绝没有 start code、空 NAL、仅有 NAL Header、`forbidden_zero_bit != 0` 或保留/未定义 NAL
  type；
- 不解析 slice header，不重新判断 `key_frame`，也不修改 SPS、PPS、SEI、AUD 或 VCL 内容。

SPS/PPS 是否出现以及出现频率由编码器配置决定。RTP Backend 只按编码器给出的 AU 原样发送，不
缓存、不合并、不主动重复参数集。

## 8. RTP Header 与会话状态

首版 RTP Header 固定为 12 字节：

```text
Version = 2
Padding = 0
Extension = 0
CSRC Count = 0
Marker = 按 AU 边界设置
Payload Type = config.payload_type
Sequence Number = session.next_sequence
Timestamp = 当前 AU 的 90 kHz timestamp
SSRC = 当前会话固定值
```

所有多字节字段使用网络字节序。首版不携带 CSRC、Header Extension 或 RTP padding。

### 8.1 Sequence

- 初始值在 `open()` 时随机生成；
- 每成功构造一个准备发送的 RTP 包分配一个 sequence；
- 每个后续包加一，按 16 bit 自然回绕；
- 同一 NAL 的 FU-A 分片和同一 AU 的不同 NAL 共用同一连续序列空间；
- 发送失败后会话终止，不尝试复用或倒退已经分配的 sequence。

### 8.2 Timestamp

H.264 RTP 时钟固定为 90 kHz。同一个 AU 产生的所有 RTP 包使用同一个 timestamp：

```text
rtp_timestamp = initial_timestamp
              + round(presentation_time_ns * 90000 / 1_000_000_000)
              modulo 2^32
```

换算使用整数运算并显式处理舍入与溢出，不通过浮点数累计。媒体时间存在调度缺口时，RTP
timestamp 保留相同缺口；不能为制造连续 timestamp 而压缩 `presentation_time`。初始 timestamp
每次 `open()` 随机生成，按 32 bit 自然回绕。

### 8.3 SSRC

- 每次 `open()` 为视频轨道随机生成一个非固定 SSRC；
- 一次打开期间所有 RTP 包使用同一个 SSRC；
- `close()` 后再次 `open()` 视为新 RTP 会话并生成新值；
- 首版不实现 SSRC 冲突检测，进入 RTCP 阶段时补充。

### 8.4 Marker

Marker 只在当前 AU 的最后一个 RTP 包上置 `1`：

- 最后一个 NAL 使用 Single NAL 时，该包置 `1`；
- 最后一个 NAL 使用 FU-A 时，最后一个 FU-A 分片置 `1`；
- SPS、PPS、SEI 或 AUD 只要不是 AU 中最后一个 NAL，Marker 都为 `0`；
- 一个 AU 恰好产生一个 Marker 包。

Receiver 可以用 Marker 加速判断 AU 边界，但仍必须结合 timestamp、sequence 和不完整数据策略，
不能假定网络一定送达 Marker 包。

## 9. H.264 RTP Payload

首版使用 RFC 6184 non-interleaved packetization mode，对外等价于 `packetization-mode=1`。仅实现：

- Single NAL Unit Packet；
- FU-A，NAL type 28。

首版不实现 STAP-A、STAP-B、MTAP、FU-B、DON 或交错发送。

### 9.1 Single NAL

设：

```text
max_rtp_payload = max_datagram_bytes - 12
```

当完整 NAL（包含一字节 NAL Header，不包含 Annex-B start code）长度小于等于
`max_rtp_payload` 时，RTP payload 就是完整 NAL，不增加 H.264 payload header。

### 9.2 FU-A

当 NAL 大于 `max_rtp_payload` 时，去掉原始一字节 NAL Header，把剩余 payload 按以下上限切分：

```text
max_fu_payload = max_datagram_bytes - 12 - 2
```

每个分片 RTP payload 为：

```text
FU Indicator | FU Header | NAL payload fragment
```

字段规则：

```text
FU Indicator.F   = original_nal_header.F
FU Indicator.NRI = original_nal_header.NRI
FU Indicator.Type = 28

FU Header.S = 1，仅第一个分片
FU Header.E = 1，仅最后一个分片
FU Header.R = 0
FU Header.Type = original_nal_header.Type
```

原始 NAL Header 不进入 FU payload。把连续 FU-A 的 fragment 字节拼接并恢复原 NAL Header 后，
必须与输入 NAL 完全一致。禁止产生空 fragment；一个 FU-A NAL 至少产生两个分片，否则应走
Single NAL 路径。

## 10. UDP 发送语义

每个 RTP 包对应一个 UDP 数据报。Socket 由 Output Worker 线程独占，无需跨线程锁。

首版规则：

- 使用固定远端的单播 UDP；
- 不绑定固定本地端口，由操作系统选择源地址与临时端口；
- Socket 采用非阻塞模式；
- 一个数据报只调用一次 `send`；
- 成功返回长度必须等于 RTP 数据报长度，否则视为发送失败；
- `would-block`、网络不可达、消息过大及其他 Socket 错误均不重试，直接使会话失败；
- 不做应用层分包之外的 IP 分片控制，避免 IP 分片主要依赖默认 1200 字节上限；
- 不在每个包上记录普通日志，只在首次失败和周期统计中报告。

不重试是首版的有意选择：重试会阻塞 Output Worker、增加直播延迟，并可能在稍后发送旧 AU。
同时，单独丢弃一个已编码 AU 会破坏后续参考帧。当前策略是在本地 UDP 栈无法及时接受数据时
终止会话，让上层明确看到失败；后续拥塞与恢复策略必须结合关键帧请求、反馈和 Receiver 行为
重新设计。

Windows 使用 Winsock，Linux CI 使用 POSIX Socket。平台差异只存在于私有 `UdpSocket` 实现和
native error code 映射中，不能泄漏进 Worker 或公共 Backend 契约。

## 11. Receipt、错误与统计

一次 AU 全部发送成功后：

```text
VideoOutputReceipt.emitted_units = 成功发送的 RTP/UDP 数据报数量
VideoOutputReceipt.emitted_bytes = 交给 UDP 的字节总数
```

`emitted_bytes` 包含 RTP Header 和 RTP payload，不包含 UDP、IP、以太网 Header。输入 H.264 字节
继续由 `VideoOutputWorkerStats::input_bytes` 记录，因此不能把两者混称为“编码码率”。

错误映射保持现有通用契约：

- 配置、地址、随机源或 Socket 创建失败：`VideoOutputOperation::Open`；
- AU/NAL 格式、RTP 封包或 UDP 发送失败：`VideoOutputOperation::Consume`；
- 非法重复调用：`VideoOutputOperation::State`；
- `close()` 幂等且不抛异常，无法通过返回值上报的关闭错误只进入低频日志。

Socket 错误保留平台 native code；纯校验或 packetizer 错误使用 `native_code = 0` 和稳定、可测试的
错误文本。`consume()` 失败不返回部分 receipt，但错误上下文应包含本 AU 已成功发送的数据报数量，
便于解释 Receiver 看到的不完整 AU。

既有 Worker 通用统计已经可以导出：

- RTP 包数和 RTP/UDP payload 字节数；
- UDP 发送失败及最后一个 native code；
- AU 输入字节与 RTP 输出字节的封装开销；
- 采集到 UDP 发送完成的平均和最大延迟。

Single NAL 包数、FU-A 包数、分片 NAL 数和最大单 AU 包数先通过 Packetizer 测试验证，不作为
首版跨 Backend 的运行时统计。如果后续确实需要结构化导出这些协议细分字段，再单独扩展通用
统计契约；不能把具体 Backend 指针暴露给 Composition 或 Main 查询。

## 12. 内存与性能

NAL Splitter 只创建 view。Packetizer 使用容量不超过 `max_datagram_bytes` 的复用 scratch buffer，
逐包构造、发送并复用，不为一个大 AU 保留 `vector<vector<byte>>`。

为了让纯算法可测试，Packetizer 可以接收同步 emitter callback：

```text
packetize(access_unit, session_state, emit_datagram)
```

生产 emitter 立即调用 UDP Socket；测试 emitter 把数据报复制到测试容器。callback 仅是 Backend
模块内部协作方式，不进入 `VideoAccessUnitOutputBackend`。

首版接受每个 RTP 包一次从 NAL view 到连续数据报 buffer 的复制。只有性能测量证明这里成为
瓶颈时，才评估 scatter/gather I/O；不能在首版把平台 `WSABUF`/`iovec` 暴露到 Packetizer 接口。

## 13. 测试策略

### 13.1 NAL Splitter

- 单个和多个 NAL；
- 三字节、四字节以及混合 start code；
- SPS/PPS/SEI/AUD/VCL 顺序保持；
- leading/trailing zero bytes；
- 无 start code、空 NAL、仅 Header、非法 F bit 和非法 type；
- view 指向原 AU 且不复制 payload。

### 13.2 Packetizer

- NAL 长度小于、等于和大于 Single NAL 边界；
- FU-A 第一个、中间和最后一个分片的 S/E/R、NRI 和 Type；
- 重组后与原 NAL 完全一致；
- 每个数据报都不超过 `max_datagram_bytes`；
- 多 NAL AU 共享 timestamp，只有最后一个包设置 Marker；
- sequence 连续及 `65535 -> 0` 回绕；
- 90 kHz 换算的整数舍入、调度缺口和 32 bit timestamp 回绕；
- PT、SSRC、网络字节序和固定 RTP Header 字段；
- 空 AU、畸形 Annex-B 和过小数据报配置失败。

这些测试使用固定 RTP 初始状态，不依赖系统随机数和网络。

### 13.3 Backend 与 UDP loopback

- open/consume/flush/close 正常生命周期和重复调用；
- IPv4 与平台支持时的 IPv6 loopback；
- loopback 接收的数据报与 Packetizer 期望完全一致；
- 一个大 NAL 实际产生多个 UDP 数据报；
- receipt 包数和字节数与接收结果一致；
- 无效 endpoint、端口、PT、数据报大小和 Socket 错误映射；
- 发送中途失败后进入 `Failed`，只能 close；
- close 幂等，重新 open 生成新的 RTP 会话状态。

普通 CI 只使用 loopback 或内部测试 emitter，不依赖外部网络服务。

### 13.4 无设备链路与人工验收

自动化链路：

```text
SyntheticDesktopCaptureBackend
-> VideoCaptureWorker
-> FfmpegH264EncoderBackend
-> VideoOutputWorker
-> RtpUdpVideoOutputBackend
-> loopback receiver
-> RTP Header 校验 + FU-A 重组 + H.264 字节比对
```

人工验收：

- Publisher 向本机或局域网测试 Receiver 连续发送；
- 使用 Wireshark 或 SemiStreamProbe 验证 PT、SSRC、sequence、timestamp、Marker 和 FU-A；
- 接收端重组后得到可由标准工具解析的 Annex-B H.264；
- 统计 RTP 包数、封装开销、发送错误和采集到发送延迟；
- 连续运行 30 分钟，无持续内存增长，timestamp 和 sequence 回绕测试另由单元测试覆盖。

## 14. 实现顺序

1. 增加包含必选 RTP 和可选调试文件的 Publisher 输出配置；
2. 实现并测试 Annex-B NAL Splitter；
3. 实现并测试 RTP Header、时间映射和 H.264 Single NAL/FU-A Packetizer；
4. 实现 Windows/POSIX 私有 UDP Socket 与 loopback 测试；
5. 实现 `RtpUdpVideoOutputBackend` 生命周期、receipt 和错误映射；
6. Composition 始终注入 RTP Backend，增加 RTP CLI；
7. 增加 Synthetic -> FFmpeg -> RTP/UDP 的无设备集成测试；
8. 使用 Wireshark/SemiStreamProbe 完成人工交叉验证；
9. 增加 Backend 内部的可选 `H264FileRecorder`，验证其失败不影响 RTP；
10. RTP 主输出稳定后，再设计 Receiver 重组和 Relay 转发。

## 15. 已决定

- 视频 RTP/UDP 由 infrastructure 的 `RtpUdpVideoOutputBackend` 实现；
- Composition 创建具体 Backend，并通过 `VideoAccessUnitOutputBackend` 注入
  `DefaultVideoOutputWorker`；
- 正式主输出始终为 RTP/UDP，不提供文件与 RTP 互斥切换模式；
- 文件输出只是可选、异步、best-effort 调试旁路，失败不得反压或终止 RTP；
- `H264FileOutputBackend` 只保留用于 M1 历史验收和隔离测试，不进入目标 Composition；
- Output Worker 不理解 RTP、H.264 分片、endpoint 或 Socket；
- RTP Backend 同步处理一个完整 AU，不创建发送线程和额外队列；
- 首版使用 RTP/UDP、90 kHz 时钟、动态 PT、随机初始 sequence/timestamp/SSRC；
- 首版使用 RFC 6184 non-interleaved 模式，只实现 Single NAL 和 FU-A；
- 一个 AU 的全部包使用同一 timestamp，只有最后一个包设置 Marker；
- 默认 UDP 数据报上限为 1200 字节，包含 RTP Header；
- UDP 发送有界且不重试；发送失败使当前发布会话失败；
- Receipt 的 unit 是 RTP/UDP 数据报，bytes 是不含 UDP/IP Header 的数据报字节；
- 首版不实现 RTCP、STAP、重传、FEC、拥塞控制、SDP、信令或多目标发送；
- 若以后需要异步/paced/retransmission 发送，再重新评估专用 Sender Worker。
