# SemiLive 文档导航

本文档目录按内容类型组织。只在出现实际文档时创建子目录，避免提前建立空层级。

## 路线图

- [项目路线图](roadmap.md)：项目阶段、交付物和完成条件。

## 设计

### Publisher

- [Publisher 音视频设计总览](design/publisher/overview.md)：模块、线程、资源、共享时间轴、
  生命周期和测试边界。
- [FrameScheduler 设计](design/publisher/frame-scheduler.md)：视频帧 deadline、媒体时间、
  晚到跳帧及其与 VideoCaptureWorker 的协作。
- [DesktopCaptureBackend 设计](design/publisher/desktop-capture-backend.md)：桌面输出选择、CPU
  BGRA 图像契约、DXGI 采集与恢复、鼠标指针合成及 Synthetic 测试后端。
- [VideoCaptureWorker 设计](design/publisher/video-capture-worker.md)：常驻采集线程、Controller
  控制接口、固定帧率采集循环、重复帧、恢复上限和错误传播。
- [视频编码阶段设计](design/publisher/video-encoding.md)：BGRA 预处理与 H.264 Backend 边界、
  Encoder Worker 线程、AU 背压、Drain/Abort 停止语义和性能演进条件。

## 后续文档类型

需要时再增加以下目录：

- `adr/`：记录重要技术选择、备选方案和选择理由；
- `testing/`：测试计划、弱网矩阵和稳定性验证方法；
- `reports/`：带日期的性能、稳定性和兼容性测试结果；
- `design/relay/`、`design/receiver/`：对应程序进入设计阶段后创建。

设计细节只在所属组件文档维护。总览保留约束摘要并链接详细设计，避免同一算法存在两份
相互独立的描述。
