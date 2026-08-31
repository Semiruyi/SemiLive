# SemiLive

SemiLive 是一个正在开发的 C++23 实时音视频项目，目标是完成桌面和系统音频采集、
编码、网络传输、Linux 转发以及接收播放的可验证闭环。播放侧计划复用
[SemiPlayer](../SemiPlayer)，码流分析经验来自
[SemiStreamProbe](../SemiStreamProbe)。

## 当前状态

项目目前只有工程骨架，尚未实现采集、编码或网络传输能力。现有三个可执行程序仅用于
验证 Windows/Linux 构建、测试和持续集成基线：

- `semilive_publisher`：发布端占位程序；
- `semilive_relay`：Linux 转发端占位程序；
- `semilive_receiver`：接收端占位程序。

Publisher 首版音视频模块边界、线程模型、共享时间轴和运行语义已经确定，实施仍先完成
视频闭环，详见 [Publisher 音视频设计](docs/publisher-design.md)。Receiver 和 Relay 仍将在进入对应
里程碑前单独设计。

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
- 多种编解码格式与协议同时支持；
- Linux 桌面采集和完整播放器移植；
- 生产级鉴权、集群调度和运维平台；
- 自研音视频编码器。

## 设计与路线图

- [Publisher 音视频设计](docs/publisher-design.md)：发布端双轨模块、线程、时间轴、依赖与测试边界；
- [项目路线图](docs/roadmap.md)：项目阶段、交付物和完成条件。

## 构建

Windows 开发环境使用 MSYS2 UCRT64。打开 **MSYS2 UCRT64** 终端，用一条命令完成
系统更新并安装编译器、CMake、Ninja 和 spdlog：

```sh
pacman -Syu --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-spdlog
```

仍在 MSYS2 UCRT64 终端中配置、构建并测试：

```sh
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Linux 开发环境需要 C++23 编译器、CMake、Ninja 和 spdlog：

```sh
sudo apt-get update
sudo apt-get install --yes ninja-build libspdlog-dev
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

运行占位程序：

```sh
./build/windows-debug/bin/semilive_publisher.exe --help
./build/windows-debug/bin/semilive_relay.exe --help
./build/windows-debug/bin/semilive_receiver.exe --help
```

## 许可证

项目源码采用 [MIT License](LICENSE)。FFmpeg、libx264 等外部依赖分别遵循其自身许可证，
不包含在本项目的 MIT 授权范围内；分发包含这些依赖的二进制文件时，需要同时满足对应的
许可证要求。
