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

具体内部架构将在实现每个里程碑前单独设计，不在工程骨架阶段预设。

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

## 路线图

详细的阶段目标和验收条件见 [项目路线图](docs/roadmap.md)。

## 构建

Windows 开发环境使用 MSYS2 UCRT64、CMake 和 Ninja：

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Linux 开发环境需要 C++23 编译器、CMake 和 Ninja：

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

运行占位程序：

```powershell
.\build\windows-debug\semilive_publisher.exe --help
.\build\windows-debug\semilive_relay.exe --help
.\build\windows-debug\semilive_receiver.exe --help
```

## 许可证

项目许可证尚未确定。在明确许可证前，请勿将本仓库代码视为已获得开源授权。

