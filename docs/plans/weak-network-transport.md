# RTCP 反馈与丢包恢复实施计划

本文定义 SemiLive 在现有 H.264/RTP/UDP 视频链路上增加 RTCP、NACK/RTX 和 PLI 的范围、
实施顺序、交付物和完成条件。它是实施计划，不表示这些能力已经完成。

## 1. 目标

当前 Receiver 可以在有限窗口内恢复乱序；确认连续性破坏后，会丢弃受损数据并等待下一个完整
SPS/PPS/IDR 随机访问点。下一阶段在不引入完整 WebRTC 的前提下建立双向反馈闭环：

```text
Publisher -- RTP/H.264 + RTCP SR --> Receiver
Publisher <-- RTCP RR/NACK/PLI ----- Receiver
Publisher -- RTP/RTX or new IDR ---> Receiver
```

目标是回答并用数据验证四个问题：

1. Receiver 如何把丢包、抖动和接收时间反馈给 Publisher；
2. 哪些缺包仍值得重传，哪些已经超过播放截止时间；
3. 重传无法恢复时，如何主动请求新的随机访问点；
4. 恢复收益需要多少额外带宽、内存和延迟。

## 2. 首版范围

首版实现：

- RTCP 公共头、复合包边界和长度校验；
- RFC 3550 Sender Report、Receiver Report 和 Report Block；
- 使用 LSR/DLSR 计算 RTT；
- RFC 4585 Generic NACK 和 Picture Loss Indication；
- 有界的发送历史缓存、NACK 重试与截止时间；
- RFC 4588 RTX 发送与接收还原；
- PLI 驱动的强制 IDR，以及请求冷却和合并；
- 基于标准弱网工具的可复现实验环境、运行统计和对照报告。

首版不实现：

- FEC、FlexFEC 或 RED；
- TWCC、REMB、GoogCC 或其他拥塞控制；
- 动态分辨率、帧率或编码码率调整；
- RTCP mux、NAT 穿透、SRTP 或完整 WebRTC；
- 音频 NACK、音频 PLC 和自适应音频 jitter buffer；
- 多路 SSRC、Simulcast 或 SVC。

## 3. 控制通道边界

首版使用独立 RTP 和 RTCP UDP endpoint，不实现 RTP/RTCP mux。Publisher 与 Receiver 均显式
配置本地 RTCP bind endpoint 和远端 RTCP peer endpoint，避免依赖隐含的端口加一规则。

网络 I/O 与协议逻辑继续分离：

- Infrastructure 负责 UDP 收发和单调到达时间；
- RTCP Parser/Serializer 只处理拥有型字节和协议值；
- 反馈调度器决定何时发送 RR、NACK 或 PLI；
- 发送历史缓存不持有 Socket，也不调用编码器；
- Publisher Controller 协调重传请求和关键帧请求的生命周期。

所有缓存必须同时具有时间和容量上限。停止会话时取消定时器、清空缓存并终止尚未发送的反馈，
不能让旧会话反馈影响下一次会话。

## 4. 实施顺序

### Phase A：基线和弱网实验环境

- 冻结当前无反馈 UDP 链路的统计字段；
- Windows 本地开发和首轮对照实验使用 `semilive_relay`，以固定 seed 注入随机丢包并输出
  机器可读统计；
- 具备 Linux 网关环境后使用流量路径上的 `tc netem`，交叉验证随机/突发丢包、延迟、抖动、
  乱序、重复包和带宽限制；
- 使用 Wireshark 抓包校验实际流量、RTP 序列号变化和反馈包，不把工具配置值直接当作实测值；
- 对固定输入记录受损 AU、随机访问恢复时间、输出 AU 数和额外带宽；
- 保存工具版本、完整命令或配置、网络拓扑、输入媒体、构建版本、随机种子（工具支持时）和原始
  抓包/统计。

`semilive_relay` 保持为应用层、UDP 无感知的实验工具，不承担媒体协议处理，也不宣称等价于
内核网络仿真。首轮报告通过 Relay、抓包和接收端统计交叉验证；未来具备 Linux 环境后再增加
`tc netem` 结果，不阻塞当前 RTCP/NACK/RTX 主线。

完成条件：弱网配置可以用已记录的命令重复建立和清理；工具只影响目标测试流量；Clean 与选定
弱网场景均能重复运行并得到定义一致的无反馈基线。

### Phase B：RTCP SR/RR

- 实现 RTCP 公共头、复合包遍历和未知包跳过；
- Publisher 周期发送 SR：NTP、RTP timestamp、包数和字节数；
- Receiver 周期发送 RR：fraction lost、cumulative lost、extended highest sequence、jitter、
  LSR 和 DLSR；
- Publisher 根据 SR/RR 往返计算 RTT；
- 对序列号回绕、负 cumulative lost、NTP compact format 和非法长度增加测试；
- 使用 Wireshark 对照字段和时间单位。

完成条件：两端可以持续交换 SR/RR；RTT、丢包率和 jitter 有明确单位、采样周期和无数据语义。

### Phase C：Generic NACK 和发送历史缓存

- Reorder Buffer 暴露“候选缺包、缺包补齐、最终确认丢失”的清晰事件；
- Missing Packet Tracker 合并连续缺包并编码 PID/BLP；
- 候选缺包先经过短暂乱序等待，再根据 RTT、重试次数和媒体截止时间决定首次 NACK、重试和放弃；
- 只有超过恢复截止时间或容量边界后才向解包管道确认连续性破坏，不能在仍允许 RTX 恢复时提前
  丢弃当前 AU；
- Publisher 使用扩展序列号索引有界历史缓存；
- 统计 NACK 请求、缓存命中、过期、重复请求和实际重发；
- 对回绕、迟到原包与重传包竞争、NACK 风暴和停止会话增加测试。

完成条件：符合资格的候选缺包能够触发有限次数的 NACK；原包或 RTX 在截止时间前到达时仍可
恢复当前 AU；不存在无界重试和无界缓存；原包迟到后不会继续请求同一包。

### Phase D：RFC 4588 RTX

- 为 RTX 配置独立 Payload Type、SSRC 和序列号空间；
- RTX payload 前两个字节携带原始 RTP sequence number；
- Receiver 还原原始 RTP 头语义，并把包重新送入现有重排管道；
- 重传包不得绕过 Session Filter、重排、解包和 AU 完整性检查；
- 记录 RTX 包数、字节数、恢复数和迟到丢弃数。

完成条件：抓包可以区分原始 RTP 与 RTX；还原后的包与原包共享同一连续性判断；超过截止时间的
RTX 不增加播放延迟。

### Phase E：PLI 与关键帧恢复

- Recovery Gate 或反馈策略在不可恢复的参考帧损坏后请求 PLI；
- Publisher 编码契约增加显式 `request_keyframe` 能力；
- x264 后端在下一可行帧产生 IDR，并在恢复 AU 中携带必要参数集；
- 对重复 PLI 进行合并和冷却，避免关键帧风暴；
- 区分“RTX恢复”“PLI恢复”和“等待周期IDR恢复”的统计。

完成条件：NACK 已无恢复价值时可以主动获得新的随机访问点；连续 PLI 不造成无界码率突发；
恢复原因可从日志和统计中确定。

### Phase F：报告与发布

- 按弱网测试矩阵运行无反馈、NACK/RTX、NACK/RTX+PLI 三组对照；
- 记录恢复率、恢复时间、损坏 AU、额外字节、端到端延迟和资源占用；
- 发布原始数据、汇总表、运行命令、构建版本和已知限制；
- 录制丢包注入、反馈、恢复和统计输出的短演示。

完成条件：报告中的每个数字都能由公开命令和固定输入复现；README 只把已经验收的能力描述为
当前能力。

## 5. 必须暴露的统计

| 分类 | 最低字段 |
|---|---|
| RTP接收 | received、reordered、duplicate、late、confirmed lost |
| RTCP | SR/RR发送与接收、解析失败、当前RTT、当前jitter、报告丢包率 |
| NACK | 首次请求、重试、合并序列号、取消、超过截止时间 |
| 历史缓存 | 当前包数/字节数、峰值、命中、未命中、过期 |
| RTX | 发送包/字节、接收包、成功还原、恢复、迟到丢弃 |
| PLI | 发送、接收、合并、冷却拒绝、强制IDR |
| 视频恢复 | 损坏AU、恢复 episode、等待随机访问总/最大时长、RTX恢复点、PLI恢复点、周期IDR恢复点 |

统计字段应绑定会话世代；重启会话后不混入上一次会话数据。

## 6. 关键工程约束

- 不把“等待下一个 IDR”表述为重传或 RTX；
- 不把原 RTP 包原样重发称为 RFC 4588 RTX；
- jitter 统计是网络到达变化，不等同于播放 jitter buffer；
- NACK 决策同时考虑乱序等待、RTT、缓存可用性和播放截止时间；
- PLI 是恢复手段，不是每次丢包的默认动作；
- 所有结论来自对照实验，不以单次演示代替稳定性结果。

## 7. 参考标准

- RFC 3550：RTP 与 RTCP；
- RFC 4585：RTP/AVPF 反馈、Generic NACK 与 PLI；
- RFC 4588：RTP Retransmission Payload Format；
- RFC 6184：H.264 RTP Payload Format。
