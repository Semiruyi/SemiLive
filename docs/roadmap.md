# SemiLive 项目路线图

本文只维护项目阶段、当前状态和完成条件。具体设计与任务拆分放在对应设计文档或实施计划中。
勾选表示仓库中已经存在可验证实现，不表示整个里程碑已经完成。

## Milestone 0：工程基线

- [x] C++23 与 CMake 工程骨架
- [x] 发布端、转发端和接收端占位入口
- [x] Windows 与 Linux 构建预设
- [x] 基础 CTest 冒烟测试
- [x] Windows 与 Linux CI 配置
- [x] 明确 Publisher 首版音视频模块、线程、共享时间轴和依赖设计

完成条件：在 Windows 和 Linux CI 中均可配置、构建并通过全部测试。

## Milestone 1：桌面视频采集与编码

- [x] 使用 DXGI 采集 Windows 桌面和鼠标指针
- [x] 支持固定分辨率与帧率调度
- [x] 使用 FFmpeg/libx264 编码 H.264
- [x] 支持编码结果的文件调试输出
- [x] 输出帧率、码率和编码会话统计
- [ ] 完成 1080p30、30 分钟稳定性验证

完成条件：标准工具可以播放输出码流，时间戳单调，无持续内存增长。

## Milestone 2：H.264/RTP/UDP 文件接收闭环

- [x] Publisher 实时发送编码后的视频
- [x] 按 RFC 6184 发送 Single NAL 和 FU-A
- [x] Receiver 监听 UDP 并校验 RTP 会话
- [x] 使用有界时间和容量恢复小范围乱序
- [x] Receiver 重组 Single NAL、FU-A 和完整 Access Unit
- [x] 连续性破坏后等待完整 SPS/PPS/IDR 随机访问点
- [x] 输出 Annex-B H.264 文件和接收统计
- [ ] 使用 Wireshark 或标准播放器交叉验证

完成条件：Publisher 与 Receiver 可以连续运行 30 分钟；输出码流可由标准工具解码；
RTP 包数、丢包、乱序、恢复点和输出 AU 统计能够相互解释。

## Milestone 3：RTCP 反馈与丢包恢复

- [ ] 建立可复现的丢包、延迟、抖动、乱序和重复包注入能力
- [ ] 实现 RTCP Sender Report 与 Receiver Report
- [ ] 统计 RTT、丢包率、interarrival jitter 和反馈状态
- [ ] 实现 Generic NACK、发送历史缓存和重传截止时间
- [ ] 按 RFC 4588 实现 RTX 封装与接收还原
- [ ] 实现 PLI、发送端强制 IDR 和请求抑制
- [ ] 对比等待周期 IDR、NACK/RTX 和 PLI 三种恢复策略
- [ ] 发布弱网与恢复报告

完成条件：选定的弱网场景均有可重复建立和清理的工具配置；重传与关键帧请求不会无界占用内存或带宽；
报告使用实际数据说明每种策略的恢复收益、额外带宽和延迟代价。具体范围见
[RTCP 反馈与丢包恢复计划](plans/weak-network-transport.md)和
[弱网测试矩阵](testing/weak-network-matrix.md)。

## Milestone 4：实时播放闭环与 v0.1.0

- [ ] 为 SemiPlayer 增加版本化的实时 H.264 AU 输入接口
- [ ] 实现 Receiver 的 SemiPlayer 输出适配器
- [ ] 完成 Publisher -> Receiver -> SemiPlayer 实时播放
- [ ] 测量启动到首帧和端到端延迟
- [ ] 完成 1080p30、30 分钟稳定性验证
- [ ] 提供少步骤启动命令和 60 至 90 秒演示
- [ ] 发布 Windows x64 v0.1.0

完成条件：新用户可以按照 README 复现实时视频闭环，连续播放 30 分钟且没有持续资源增长，
并能查看首帧时间、端到端延迟和接收统计。

## Milestone 5：WebRTC Native 实践

- [ ] 固定并可复现构建一个 libwebrtc 版本
- [ ] 将 Windows 桌面采集接入 C++ Native `PeerConnection`
- [ ] 与浏览器完成 H.264 视频互通
- [ ] 实现最小 WebSocket 信令和 Trickle ICE
- [ ] 验证 host、srflx 和 relay Candidate 路径
- [ ] 使用 coturn 验证强制 TURN 中继
- [ ] 采集连接、传输、丢包、NACK、PLI、码率和首帧统计
- [ ] 完成至少一项可复现的源码级调用链或小型改动
- [ ] 发布互通演示和弱网数据

完成条件：新用户可以复现 C++ Native 到浏览器的直连与 TURN 中继；文档能够解释
SDP、ICE、DTLS-SRTP 和媒体反馈的实际数据流；至少一项源码级工作具有测试或数据证据。
具体范围见 [WebRTC Native 实践计划](plans/webrtc-native.md)。

## Milestone 6：Linux 视频网络链路

- [ ] 转发程序在 Ubuntu 上构建和运行
- [ ] 支持一个发布者与多个播放端
- [ ] 管理会话创建、超时与释放
- [ ] 输出每路流的实时统计
- [ ] 支持优雅退出
- [ ] 增加 Linux 自动化测试与运行时检查

完成条件：Windows 发布端可经 Linux 转发到至少两个播放端，持续运行 30 分钟无明显资源泄漏。

## Milestone 7：系统音频与音画同步

- [ ] 采集 Windows 系统音频
- [ ] 编码、发送、接收并播放音频
- [ ] 实现已经设计的统一音视频时间线和轨道映射
- [ ] 接入 SemiPlayer 音频输出与同步
- [ ] 处理静音、短暂断流和设备不可用
- [ ] 记录音视频同步差值和长期漂移

完成条件：音视频可经完整网络拓扑连续播放 30 分钟且没有明显累计漂移，短暂断流后可恢复。

## 后续候选方向

Milestone 7 完成后再根据实际岗位和使用场景选择，不并行展开：

- 自适应 jitter buffer、带宽估计、Pacer 和动态编码码率；
- FEC 与 NACK/FEC 混合恢复；
- SFU、多路流压力测试与服务监控；
- 安防协议和设备接入；
- 更多采集设备与客户端 SDK 能力。
