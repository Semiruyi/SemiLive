# SemiLive

SemiLive 是一个正在开发的 C++23 实时音视频项目，目标是完成桌面和系统音频采集、
编码、网络传输、Linux 转发以及接收播放的可验证闭环。播放侧计划复用
[SemiPlayer](../SemiPlayer)，码流分析经验来自
[SemiStreamProbe](../SemiStreamProbe)。

## 当前状态

项目已经接通从 Windows 桌面采集到 H.264/RTP/UDP 文件接收的首条视频链路：

- `semilive_publisher`：采集 Windows 桌面并向指定 UDP endpoint 发送 H.264 RTP；
- `semilive_receiver`：监听单路 H.264/RTP/UDP，完成 RTP 校验、有限重排、
  Single NAL/FU-A 解包、Access Unit 组装和随机访问恢复，并输出 Annex-B 文件；
- `semilive_relay`：Linux 转发端仍是占位程序。

当前 Receiver 输出仍是阶段验证用的 Annex-B 文件，尚未接入 SemiPlayer 实时播放；系统音频、
Linux Relay、RTCP 反馈、重传和 WebRTC 也尚未实现。当前能力和后续阶段以
[项目路线图](docs/roadmap.md)为准。

## 项目目标

- 在 Windows 采集桌面视频和系统音频并完成实时编码；
- 通过网络传输实时音视频；
- 在 Linux 上运行媒体转发程序；
- 接收媒体并交给 SemiPlayer 解码、同步和播放；
- 对丢包、乱序、抖动、首帧时间和端到端延迟提供可复现的数据；
- 建立 Windows 与 Linux 自动化构建和测试。

## 非目标

首个可用版本不计划实现：

- 完整 WebRTC 协议栈；
- NAT 穿透和公网信令；
- 拥塞控制、FEC 和生产级弱网算法；
- 多种编解码格式与协议同时支持；
- Linux 桌面采集和完整播放器移植；
- 生产级鉴权、集群调度和运维平台；
- 自研音视频编码器。

## 设计与路线图

- [文档导航](docs/README.md)：设计、决策、测试与报告入口；
- [Publisher 音视频设计](docs/design/publisher/overview.md)：发布端双轨模块、线程、时间轴、依赖与测试边界；
- [Receiver 视频设计](docs/design/receiver/overview.md)：接收端 RTP/H.264、有限重排和随机访问恢复；
- [项目路线图](docs/roadmap.md)：项目阶段、交付物和完成条件。

## 构建

Windows 开发环境使用 MSYS2 UCRT64。打开 **MSYS2 UCRT64** 终端，用一条命令完成
系统更新并安装编译器、CMake、Ninja、pkg-config、spdlog 和 FFmpeg 开发库：

```sh
pacman -Syu --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-spdlog \
  mingw-w64-ucrt-x86_64-ffmpeg
```

仍在 MSYS2 UCRT64 终端中配置、构建并测试：

```sh
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Linux 开发环境需要 C++23 编译器、CMake、Ninja、pkg-config、spdlog、libavcodec、
libavutil 和 libswscale：

```sh
sudo apt-get update
sudo apt-get install --yes \
  g++ cmake ninja-build pkg-config \
  libspdlog-dev libavcodec-dev libavutil-dev libswscale-dev
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

先启动 Receiver，把恢复后的 Annex-B H.264 写入文件：

```sh
./build/windows-debug/bin/semilive_receiver.exe \
  --bind-address 0.0.0.0 \
  --bind-port 5004 \
  --output semilive-received.h264 \
  --output-stall-threshold-ms 100 \
  --stats-json receiver-stats.json
```

Receiver 正常停止后会写出机器可读的会话配置、RTP 重排与确认丢包、H.264 解包与 AU 丢弃、
随机访问恢复次数/耗时，以及首个输出、输出停顿和会话结束时尾部间隔等基线指标。启动阶段等待
第一个 IDR 不计入弱网恢复 episode；未完成的恢复等待会单独记录，不冒充已完成恢复。

再启动 Publisher；按 Ctrl+C 正常停止两个进程：

```sh
./build/windows-debug/bin/semilive_publisher.exe --help
./build/windows-debug/bin/semilive_publisher.exe \
  --rtp-address 127.0.0.1 \
  --rtp-port 5004 \
  --stats-json publisher-stats.json
```

Publisher 正常停止后会写出视频/GOP/码率和 RTP 配置，以及采集帧、编码 AU、关键帧、原始媒体
RTP datagram 与字节数，作为 Receiver 丢包率和后续反馈/重传开销的发送端分母。

当前接收结果用于协议闭环验证，可使用 FFmpeg/ffprobe 检查。Relay 目前仍是占位程序：

```sh
./build/windows-debug/bin/semilive_relay.exe --help
```

## 许可证

项目源码采用 [MIT License](LICENSE)。FFmpeg、libx264 等外部依赖分别遵循其自身许可证，
不包含在本项目的 MIT 授权范围内；分发包含这些依赖的二进制文件时，需要同时满足对应的
许可证要求。
