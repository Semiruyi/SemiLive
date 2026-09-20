# 弱网测试矩阵

本文定义 SemiLive 自研 RTP/RTCP 链路和后续 WebRTC Native 链路共用的弱网实验方法。具体报告
必须记录实际环境和原始结果；本文只规定输入维度、指标定义和报告格式，不预填性能结论。

## 1. 工具与拓扑

SemiLive 当前使用项目内的单向 UDP Relay 建立可自动化、可复现的随机丢包条件：

- Windows 本地开发和首轮对照实验使用 `semilive_relay`；
- Clumsy 可用于人工交叉验证；
- 具备 Linux 网关环境后使用 `tc netem` 做系统级交叉验证；
- Wireshark 用于确认目标流量、RTP 序列变化和 RTCP 反馈；
- 工具统计、抓包统计和 SemiLive 自身统计分别记录，不能互相替代。

`semilive_relay` 是应用层故障注入器，不等同于操作系统网络队列。当前版本只实现按完整 UDP
datagram 的固定 seed 随机丢包，不模拟内核排队、链路带宽或网卡行为。报告必须同时保存配置丢包
率、Relay 实际丢弃统计和 Receiver 确认丢包，不能把任意一个数字替代另外两个。

当前测试拓扑：

```text
Publisher :5004 --> semilive_relay --> :5006 Receiver
```

测试 RTP 下行时只在 Publisher 到 Receiver 的方向施加规则；增加 RTCP 后，上行反馈方向启动
第二个 Relay 实例并使用独立端口、seed 和统计，不能用一个“RTT”配置掩盖两个方向的差异。

Clumsy 适合快速切换更多故障类型，但 localhost 流量可能被重复捕获。`tc netem` 必须位于被测
流量真实经过的 Linux 网络接口上；仅在 WSL2 中配置 qdisc，不代表 Windows 主机上的 UDP 流量
会自动经过它。

工具参考：

- Clumsy：<https://jagt.github.io/clumsy/>；
- Clumsy 使用限制：<https://jagt.github.io/clumsy/manual.html>；
- `tc netem` 手册：<https://man7.org/linux/man-pages/man8/tc-netem.8.html>。

## 2. 可复现性要求

每次正式实验必须记录：

- Git commit、构建类型、编译器和依赖版本；
- 操作系统、CPU、内存和网络拓扑；
- 输入内容、分辨率、帧率、codec、GOP和目标码率；
- 弱网工具及版本、完整命令或配置、过滤条件和随机种子（工具支持时）；
- 发送端、接收端和中间节点的时钟来源；
- 预热时间、正式测量时间和重复次数；
- 原始日志、抓包、工具统计、机器可读统计和汇总脚本版本；
- 规则应用前、应用后和清理后的 qdisc 或工具状态。

“可复现”表示其他人可以根据记录重新建立同类网络条件和得到方向一致、定义一致的结果，不要求
不同运行逐包丢失位置完全相同。支持 seed 的工具应固定并记录 seed；每个正式场景仍须重复多次，
避免把单次随机结果当作结论。

## 3. 固定基线

首份报告使用固定视频配置，变更时新建报告而不是覆盖旧结果：

- 1920x1080、30fps、H.264；
- 固定 GOP 和目标码率；
- 单路 Publisher、单路 Receiver；
- 每个场景先运行短验证，再进行不少于 10 分钟的正式测量；
- 同一场景至少重复 3 次，报告中位数和最差一次。

端到端延迟需要画面内时间源或其他可验证标记。不能用发送日志与接收日志的未校准系统时间直接
相减作为端到端延迟。

## 4. 单因素矩阵

先分别改变一个因素，确认统计和行为正确：

| 维度 | 测试值 |
|---|---|
| 随机丢包 | 0%、1%、3%、5%、10% |
| 突发丢包 | 每次连续丢 3、5、10 个包 |
| 单向基础延迟 | 0、25、50、150ms |
| 抖动 | 0、10、20、50ms |
| 乱序 | 0%、1%、5%、10% |
| 重复包 | 0%、1%、5% |
| 下行带宽 | 8、4、2、1Mbps |

随后选择有限的组合场景，避免完整笛卡尔积：

| 场景 | 丢包 | RTT目标 | 抖动 | 带宽 |
|---|---:|---:|---:|---:|
| Clean | 0% | 20ms | 0ms | 8Mbps |
| Mild | 1% | 50ms | 10ms | 4Mbps |
| Mobile | 3% | 100ms | 20ms | 2Mbps |
| Harsh | 5% | 300ms | 50ms | 1Mbps |
| Burst | 5包突发 | 100ms | 20ms | 2Mbps |

组合参数是首份报告的固定起点，不代表产品承诺。若目标码率高于链路带宽，报告必须把持续排队和
拥塞造成的丢包与注入的随机丢包分开说明。

## 5. 对照组

自研链路依次比较：

1. Baseline：有限乱序，损坏后等待周期 IDR；
2. Feedback：RTCP SR/RR，仅增加观测，不改变恢复；
3. Retransmission：RTCP + NACK/RTX；
4. Recovery：RTCP + NACK/RTX + PLI。

WebRTC 报告至少区分：

1. 局域网或公网直连；
2. 强制 TURN relay；
3. Clean、Mobile 和 Harsh 三种网络条件。

不能把不同输入、不同GOP、不同目标码率或不同测试时长的结果放在同一对照表中直接比较。

## 6. 指标定义

| 指标 | 定义 |
|---|---|
| Configured impairment | 弱网工具配置的丢包、延迟、抖动、乱序和带宽条件，不等同于实测结果 |
| Tool-reported loss | `tc -s` 等工具报告的丢包数，注明接口和方向 |
| Receiver-confirmed loss | Receiver 根据 RTP 序列连续性最终确认的丢包数 |
| Eligible missing | 在发送缓存中仍存在且预计能在播放截止时间前到达的缺包 |
| RTX recovery rate | 由 RTX 恢复并进入正常重排链路的包数 / eligible missing |
| RTX overhead | RTX 字节数 / 原始媒体 RTP 字节数 |
| Recovery time | 首次确认连续性破坏到首个恢复输出 AU 的单调时间差 |
| Damaged AU | 因缺包、FU-A损坏或不完整边界而拒绝的 AU 数 |
| Receiver output gap | Receiver 连续两次成功提交 AU 的最大墙钟间隔；文件输出阶段仅作为链路停顿代理 |
| Freeze count | 播放输出间隔超过预先声明阈值的事件数 |
| Freeze duration | 冻结事件超过正常帧间隔部分的累计时间 |
| End-to-end latency | 可验证采集标记到显示该标记的时间差 |
| First-frame time | 接收会话启动到第一个可显示视频帧的时间差 |
| Feedback overhead | RTCP 字节数 / 原始媒体 RTP 字节数 |

恢复率只针对 eligible missing，避免把已经过期或已从历史缓存淘汰的包计入分母后误解机制效果。
网络 jitter、接收重排等待和播放器缓冲延迟必须分别报告。
Receiver output gap 不等同于播放器 Freeze；接入实际播放与画面观测前，报告不得混用两个名称。

## 7. 通过条件

机制验收的最低条件：

- 弱网规则可以通过记录的命令或配置重复建立并完整清理；
- 支持 seed 的工具在固定输入下可以复现一致的故障分布，正式结论来自多次运行而非单次事件位置；
- 抓包、工具统计和 Receiver 统计对故障方向及数量级没有无法解释的矛盾；
- 畸形包和损坏媒体不会导致崩溃、越界或无界内存增长；
- NACK重试、历史缓存、RTX和PLI均有明确上限；
- 网络恢复后可以回到持续输出状态；
- Clean场景启用反馈后不存在明显功能回归；
- 所有报告数字都能追溯到机器可读原始字段；
- 未达到预设目标的场景如实记录，不从报告中删除。

性能目标在取得第一轮基线后另行冻结。没有基线前不预设“5%丢包不卡顿”等结论。

## 8. 报告目录建议

每次正式实验使用独立目录：

```text
docs/reports/YYYY-MM-DD-<scenario>/
  README.md          # 环境、命令、结论和已知限制
  config.json        # 媒体参数、网络拓扑和弱网工具配置
  runs.csv           # 每次运行的汇总值
  raw/               # 原始机器可读统计、工具输出和抓包索引
  charts/            # 由原始数据生成的图表
```

报告中的摘要表只保留对决策有用的指标，原始数据和生成方式必须同时保留。
