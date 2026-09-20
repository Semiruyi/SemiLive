# UDP 故障注入 Relay 设计

`semilive_relay` 是 SemiLive 实验环境中的单向 UDP 中转进程。它在 Publisher 与 Receiver 之间
保留完整 datagram 边界，并按固定 seed 的随机策略决定原样转发或丢弃。Relay 不解析 RTP/H.264，
也不修改 payload、序列号或时间戳。

## 1. 边界与拓扑

当前单向拓扑为：

```text
Publisher :5004 --> Relay :5004 --> Receiver :5006
```

增加 RTCP 后，反馈方向使用第二个 Relay 实例。两个方向必须使用独立端口、配置、seed 和统计，
从而分别观察媒体下行与反馈上行，不在单个进程中隐式拼成双向链路。

Relay 是应用层故障注入器，不等同于内核 qdisc、物理链路或真实拥塞队列。当前版本只提供随机
丢包；延迟、抖动、乱序、重复和限速只有在各自的调度与统计语义明确后才扩展。

## 2. 模块

- `domain/RandomLossPolicy`：以百万分率保存丢包配置，使用固定 seed 生成逐包确定性判定；
- `infrastructure/network/UdpRelaySocket`：绑定输入 endpoint，保留 datagram 边界并向固定 endpoint
  原样发送；
- `application/RelaySession`：执行接收、判定、转发循环并维护会话统计；
- `reporting/RelaySessionReport`：输出配置、有效 Socket 信息、会话结果和流量统计；
- `main.cpp`：负责 CLI、信号处理、日志和模块组装。

随机策略每收到一个 datagram 消耗一次随机样本。相同 seed、相同丢包率和相同输入包序列会得到
相同的转发/丢弃决策；输入顺序或包数变化后，不承诺与另一轮逐包位置一致。

## 3. 生命周期

1. Main 校验输入和转发 endpoint 不相同，防止本机自循环；
2. Session 打开输入 Socket、设置接收缓冲并连接输出 Socket；
3. 每轮以短超时等待一个 datagram，以便 Ctrl+C 可以及时结束；
4. 收到包后先累计输入统计，再执行丢包判定；
5. 转发失败视为会话失败，随机丢弃属于正常实验结果；
6. 停止后先关闭 Socket，再写最终 JSON 报告。

## 4. 统计口径

- `received_datagrams/bytes`：Relay 从输入 Socket 成功读取的流量；
- `forwarded_datagrams/bytes`：Relay 成功交给输出 Socket 的流量；
- `dropped_datagrams/bytes`：随机策略主动丢弃的流量；
- `actual_loss_percent`：`dropped_datagrams / received_datagrams`；
- `receive_timeouts`：无数据轮询次数，只用于运行诊断；
- `send_failures`：输出 Socket 发送失败次数，会使会话失败。

正常会话必须满足：

```text
received_datagrams = forwarded_datagrams + dropped_datagrams
received_bytes = forwarded_bytes + dropped_bytes
```

配置丢包率是概率参数，实际丢包率是本次有限样本结果，两者不要求完全相等。

## 5. 测试边界

- 0% 与 100% 丢包必须分别全量转发和全量丢弃；
- 相同 seed 必须产生相同决策序列；
- UDP 回环测试必须证明 payload 和 datagram 边界不变；
- Session 测试同时验证转发路径、丢弃路径和计数守恒；
- 报告测试覆盖配置、seed、配置丢包率和实际丢包率字段；
- CLI 测试覆盖端口、丢包率精度、重复参数和自循环拒绝。
