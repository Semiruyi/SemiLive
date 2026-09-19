# WebRTC Native 实践计划

本文定义在自研 RTP/RTCP 弱网闭环之后开展的 libwebrtc Native 实践。目标不是把 SemiLive 的
自研传输替换成不透明依赖，而是证明可以把现有采集能力接入业界标准栈、完成公网建连、解释实际
协议数据流，并进行源码级定位和验证。

## 1. 目标形态

```text
Windows C++ Native Publisher
  DXGI capture
    -> custom VideoTrackSource
    -> libwebrtc PeerConnection
    -> ICE + DTLS-SRTP + RTP/RTCP
    -> Chrome receiver

Signaling: minimal WebSocket service
ICE servers: STUN + coturn
Observability: Native stats + browser getStats + packet capture
```

首个可演示版本只要求单路桌面视频和一个发送端、一个浏览器接收端。音频、多人会议和生产级业务
后台不属于首版。

## 2. 仓库与依赖边界

- 固定 libwebrtc commit，不依赖浮动 main 分支；
- 记录 depot_tools、编译器、GN args 和目标架构；
- 提供构建/打包脚本，不把完整 libwebrtc 源码复制进 SemiLive；
- 自研 RTP/RTCP 链路与 WebRTC 链路保持独立入口，共享采集模型时通过稳定契约连接；
- coturn 配置只提交示例，不提交账号、密码、私钥或公网地址；
- 信令服务只传递 SDP、ICE candidate 和房间生命周期，不处理媒体。

## 3. 实施阶段

### Phase A：可复现构建与最小 PeerConnection

- 固定并构建 libwebrtc Native SDK；
- 创建 `PeerConnectionFactory` 和线程环境；
- 完成 Offer/Answer、Trickle ICE 和状态回调；
- 正确关闭 PeerConnection、轨道、线程和工厂；
- 用合成视频源完成同机浏览器互通。

完成条件：全新环境按照文档可以构建；重复建立和关闭连接不会遗留线程或崩溃。

### Phase B：接入 SemiLive 桌面采集

- 将 DXGI BGRA 帧映射为自定义 `VideoTrackSource` 输入；
- 明确像素转换、时间戳、帧率限制和背压发生的位置；
- 配置并验证 H.264 协商，记录实际选择的 codec、profile 和 payload type；
- 处理屏幕分辨率变化、采集暂时不可用和连接停止。

完成条件：浏览器连续显示真实桌面；采集时间戳单调；输入拥塞时内存不会无界增长。

### Phase C：信令与 ICE/STUN/TURN

- 实现最小 WebSocket 信令：房间、Offer、Answer、ICE candidate 和离开；
- 记录 local/remote description 与候选收集状态；
- 验证 host、server-reflexive 和 relay candidate；
- 部署 coturn，并使用 relay-only 策略证明媒体经过 TURN；
- 记录最终 selected candidate pair、协议、地址类型和切换事件。

完成条件：局域网直连和强制 TURN 两种场景均可复现；失败时可以从状态和日志区分信令、ICE、
DTLS 与媒体问题。

### Phase D：安全与协议证据

- 标注一份真实 SDP：media section、codec、payload type、SSRC、ICE、fingerprint 和 RTCP feedback；
- 记录 ICE connected、DTLS connected 和首个 RTP/首帧时间；
- 使用抓包证明媒体不是明文 H.264；
- 绘制 Signaling -> ICE -> DTLS -> SRTP -> first frame 的实际时序。

完成条件：文档中的字段来自一次可重复运行，而不是仅引用协议概念。

### Phase E：统计与弱网验证

- Native 和浏览器两端定期采集稳定字段；
- 至少记录 RTT、packets lost、jitter、bitrate、resolution、frames dropped、NACK、PLI、
  candidate pair 和可用发送带宽；
- 在与自研链路一致的弱网场景下运行测试；
- 分开记录直连和 TURN 的结果；
- 不直接比较定义不同的指标，报告中注明每个字段的来源和单位。

完成条件：可以将用户可见卡顿与 transport、inbound/outbound RTP 和 candidate pair 指标关联。

### Phase F：源码级交付物

至少选择一项完成，不同时展开：

1. 跟踪“采集帧 -> 编码 -> Pacer -> RTP发送”的源码调用链；
2. 跟踪“NACK接收 -> 历史包查找 -> RTX发送”的源码调用链；
3. 增加采集到发送或接收到渲染的阶段延迟统计；
4. 修改一个小型 NACK、Pacer 或关键帧请求策略，并完成 A/B 实验；
5. 使用明确的 Field Trial 切换一个传输策略并解释结果。

完成条件：记录固定 commit、涉及文件、入口条件、线程/TaskQueue、关键状态和验证数据；如果修改
源码，提供最小 patch 和回归测试。首版不修改 GoogCC 核心算法。

## 4. 交付物

- Windows C++ Native 发布端；
- 浏览器接收页面；
- 最小 WebSocket 信令服务；
- coturn 示例配置与直连/TURN运行说明；
- libwebrtc 固定版本和可复现构建脚本；
- SDP 标注文档、建连时序图和源码调用链文档；
- 弱网原始数据与汇总报告；
- 60 至 90 秒互通演示；
- 一项源码级分析或小型改动。

## 5. 非目标

- 自研 ICE、DTLS、SRTP 或完整 WebRTC 协议栈；
- 生产级鉴权、房间服务、录制和计费；
- 多人 SFU/MCU、Simulcast 或 SVC；
- Android、iOS 和 macOS 多端同时支持；
- 为了简历关键词而把 PeerConnection API 调用描述成 WebRTC 内核开发；
- 没有数据验证的拥塞控制算法修改。

## 6. 完成定义

以下条件全部满足才把“WebRTC Native 实战”写为已完成：

- C++ Native 与浏览器可以稳定互通；
- host、srflx 和 relay 路径至少各有可验证日志，TURN 场景可强制复现；
- 能用实际 SDP、状态日志和抓包解释 ICE、DTLS-SRTP 与媒体传输；
- 弱网场景有固定输入、固定参数和可复现指标；
- 至少一项源码级交付物经过测试或数据验证；
- README 明确区分自研传输能力与 libwebrtc 提供的能力。
