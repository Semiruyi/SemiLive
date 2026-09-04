# DesktopCaptureBackend 设计

本文定义 Publisher 桌面采集后端的跨平台契约、DXGI Desktop Duplication 实现边界和
Synthetic 测试后端，是 [Publisher 音视频设计总览](overview.md) 中视频采集后端的详细设计。
后端面向 `VideoCaptureWorker` 提供 CPU BGRA 桌面图像；固定帧率输出、PTS、重复帧和跨线程
发布仍由 Worker、`FrameScheduler` 与 `CapturedVideoFrameStore` 协作完成。

## 1. 目标与边界

`DesktopCaptureBackend` 负责：

- 在发布会话启动时选择并打开一个桌面输出；
- 非阻塞地查询自上次调用以来是否存在新的桌面状态；
- 将 DXGI 纹理复制为由调用方独占的 CPU BGRA 图像；
- 将旋转后的输出规范化为用户看到的正向画面；
- 按配置把鼠标指针合成到图像，真实桌面发布默认启用；
- 区分无变化、暂时不可用和致命失败；
- 在 `close()` 时释放 duplication、D3D11、映射纹理和指针缓存等资源。

后端不负责：

- 创建线程、等待帧率 deadline 或处理停止命令；
- 决定视频帧率、跳过调度点或突发补帧；
- 创建 `CapturedVideoFrame`、分配 `sequence` 或生成 PTS；
- 缓存并输出重复帧；
- 缩放到 1920 x 1080、填充黑边或转换为 YUV420P；
- 编码、RTP 封包、网络发送或更新跨阶段延迟统计；
- 在运行中切换采集输出。

`capture_latest()` 是一次快速轮询，不在 DXGI 内部等待下一次桌面更新。帧率等待统一留在
`VideoCaptureWorker`，避免 Worker 先等待调度点、再被 `AcquireNextFrame` 重复阻塞，也保证
停止请求可以由 Worker 自己的条件变量立即唤醒。

## 2. 契约数据

### 2.1 输出选择与配置

```cpp
enum class DesktopOutputSelection {
    Primary,
    Index,
};

struct DesktopOutputSelector {
    DesktopOutputSelection selection = DesktopOutputSelection::Primary;
    std::uint32_t index = 0;
};

struct DesktopCaptureConfig {
    DesktopOutputSelector output;
    bool compose_pointer = true;
};
```

`Primary` 是首版默认值。`Index` 是本次 `open()` 时对所有已连接、可附着到桌面的输出进行
确定性枚举后的零基索引，主要用于命令行选择和测试。配置只使用平台无关类型，不把
`IDXGIOutput`、Adapter LUID 或 Windows 句柄泄漏到 Worker。

打开后，DXGI 实现保存所选输出的稳定身份；发生恢复时重新寻找同一个输出，不因为枚举顺序
变化而静默切换到另一块显示器。下一次新会话重新解析 `Primary`，因此用户改变主显示器后
重新发布可以选择新的主输出。首版不支持会话运行中主动切换输出或拼接多个显示器。

### 2.2 CPU 图像

```cpp
struct DesktopImage {
    std::vector<std::byte> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
};
```

约束：

- 每像素固定 4 字节，内存顺序为 B、G、R、A；A 字节不承载透明度语义，消费者必须忽略；
- 第一行是画面顶部，`width` 和 `height` 表示用户看到的正向尺寸；
- 首版返回紧凑图像，`stride == width * 4`；仍保留 `stride`，避免调用方依赖隐含布局；
- `bgra.size() == stride * height`，所有乘法在分配前检查溢出；
- 图像拥有自己的 CPU 内存，返回后不引用 mapped staging texture 或其他后端资源；
- 空尺寸、未知像素格式或无法完整复制的画面不得作为成功图像返回。

`DesktopImage` 不包含 `sequence`、PTS 或 `captured_at`。Worker 在一次采集调用完成后，根据当前
调度点构造 `CapturedVideoFrame`，并把当时的 `steady_clock::now()` 记录为 `captured_at`。
Backend 返回值仍是独占的值对象，不承担跨线程共享策略；Worker 把其中的 BGRA vector 移入
共享不可变 `BgraFrameBuffer`，该转换不复制像素存储。

### 2.3 查询结果

```cpp
enum class DesktopCaptureOperation {
    Open,
    Acquire,
    Copy,
    Map,
    Pointer,
    Reinitialize,
};

struct DesktopCaptureIssue {
    DesktopCaptureOperation operation;
    std::int64_t native_code = 0;
    std::string message;
};

struct DesktopNoChange {};

struct DesktopTemporarilyUnavailable {
    DesktopCaptureIssue issue;
};

using DesktopCaptureObservation =
    std::variant<DesktopImage, DesktopNoChange, DesktopTemporarilyUnavailable>;

using DesktopCaptureResult =
    std::expected<DesktopCaptureObservation, DesktopCaptureIssue>;
```

四种结果语义：

| 结果 | 含义 | Worker 行为 |
|---|---|---|
| `DesktopImage` | 得到新的桌面状态，可能只变化了鼠标指针 | 替换最近画面并输出当前调度帧 |
| `DesktopNoChange` | 正常轮询时没有新状态 | 有缓存则输出重复帧，否则消耗调度点但不输出 |
| `DesktopTemporarilyUnavailable` | 后端状态已失效或输出暂不可用，但允许后续重试 | 保留最近画面、记录恢复事件并按 Worker 策略重试 |
| unexpected/error | 配置、格式、资源或 API 操作无法继续 | 关闭后端并使发布会话失败 |

`DesktopNoChange` 是正常高频结果，不携带错误信息，也不逐次写日志。暂时不可用包含原生错误码
供低频诊断，但不要求 Worker 理解 HRESULT。`std::expected` 的 error 只表达本次会话无法继续的
致命错误。内存分配等异常仍可抛出，由 Worker 线程入口统一捕获并转换为内部失败；接口不把
所有 C++ 异常强行压缩成平台错误。

## 3. 接口与生命周期草案

```cpp
struct DesktopCaptureInfo {
    std::string output_name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class DesktopCaptureBackend {
public:
    virtual ~DesktopCaptureBackend() = default;

    DesktopCaptureBackend(const DesktopCaptureBackend&) = delete;
    DesktopCaptureBackend& operator=(const DesktopCaptureBackend&) = delete;
    DesktopCaptureBackend(DesktopCaptureBackend&&) = delete;
    DesktopCaptureBackend& operator=(DesktopCaptureBackend&&) = delete;

    [[nodiscard]] virtual std::expected<DesktopCaptureInfo, DesktopCaptureIssue>
    open(const DesktopCaptureConfig& config) = 0;

    [[nodiscard]] virtual DesktopCaptureResult capture_latest() = 0;
    virtual void close() noexcept = 0;

protected:
    DesktopCaptureBackend() = default;
};
```

这是语义草案，不要求实现逐字照搬：

- `open()`、所有 `capture_latest()` 和 `close()` 必须在同一个 Video Capture Thread 调用；
- `open()` 成功前调用 `capture_latest()` 是调用方错误，实现应明确失败；
- 已打开时再次 `open()` 不隐式覆盖旧会话；必须先 `close()`；
- `close()` 幂等且不抛异常，允许启动失败和线程异常路径防御性调用；
- `close()` 后允许在同一 Worker 线程为新发布会话再次 `open()`；
- 后端不要求支持并发调用，也不在内部创建线程；
- `DesktopCaptureInfo` 只用于启动确认、日志和统计标签，不替代每张图像携带的实际尺寸。

COM 与 D3D11 初始化和释放都发生在 Video Capture Thread。Worker 在线程入口初始化 COM，
线程退出前先关闭后端，再释放 COM；后端不得把有线程亲和性的对象交给其他模块。

## 4. DXGI 实现

### 4.1 打开

`DxgiDesktopCaptureBackend::open()` 按以下顺序建立资源：

1. 枚举支持 Desktop Duplication 的适配器和已附着桌面的输出；
2. 解析 Primary 或 Index，并保存该输出的稳定身份和桌面坐标；
3. 在输出所属适配器上创建 D3D11 device 和 immediate context；
4. 查询 `IDXGIOutput1` 并调用 `DuplicateOutput`；
5. 读取输出尺寸、旋转方向和可诊断名称；
6. 清空上一会话的 staging texture、指针形状和恢复状态；
7. 返回规范化后的初始尺寸。

首版只采集一个输出。找不到目标输出、Desktop Duplication 不受支持或得到不支持的纹理格式时，
`open()` 返回带操作、原生错误码和可读信息的错误，不退回 GDI 截图，也不静默改采其他输出。

### 4.2 单次采集

正常的 `capture_latest()`：

```text
1. 如上次状态失效，做一次有界重建尝试
2. AcquireNextFrame(timeout = 0)
3. 超时 -> DesktopNoChange
4. 成功 -> 用 RAII 保证 ReleaseFrame 恰好一次
5. 取得桌面纹理并校验格式和尺寸
6. 按需要重建 CPU-readable staging texture
7. CopyResource，Map staging texture
8. 逐行复制并按输出旋转信息规范化为紧凑、正向 BGRA
9. 更新并按配置合成鼠标指针
10. Unmap，返回拥有独立内存的 DesktopImage
```

`AcquireNextFrame` 的 timeout 固定为 0。实现不得在一次调用中休眠、无限重试或等待输出恢复。
所有成功 Acquire 的路径，包括后续复制、映射或指针处理失败，都必须通过 RAII 执行一次
`ReleaseFrame`；所有成功 Map 的路径必须执行一次 Unmap。

首版每次取得新状态都复制完整画面。DXGI dirty rect、move rect、GPU 色彩转换和 GPU 纹理跨
线程传递属于测量证明 CPU 完整复制成为瓶颈后的优化，不进入初始实现。

### 4.3 旋转与尺寸变化

Desktop Duplication 图像可能按照未旋转表面提供。DXGI 后端读取输出旋转信息，并把 0、90、
180、270 度统一转换为顶部起始、用户视觉方向正确的 CPU 图像；90/270 度时交换返回的宽高。
不把 DXGI rotation 值暴露给 Worker 或编码器。

显示模式、旋转、DPI 布局或输出连接变化可能使 duplication 失效。后端丢弃依赖旧设备和旧
尺寸的全部 GPU 资源，在后续调用中重新定位同一输出并重建。恢复后的第一张图像可以具有新
尺寸；Worker 直接替换最近画面，后续 [`VideoEncoderBackend`](video-encoding.md) 内部的预处理
仍负责转换到固定 1920 x 1080 输出，因此源分辨率变化不会改变编码轨道配置。

### 4.4 鼠标指针合成

Desktop Duplication 的桌面纹理不保证包含鼠标指针。`compose_pointer == true` 时，DXGI 后端：

- 根据 frame metadata 更新指针可见性和位置；
- 在形状 metadata 到达时更新缓存，未更新时继续使用最近有效形状；
- 支持 Color、Monochrome 和 Masked Color 三类 DXGI 指针形状；
- 使用 DXGI 给出的输出相对、显示方向坐标；它与规范化旋转后的图像坐标一致；
- `PointerPosition.Position` 已表示形状左上角，`HotSpot` 不参与绘制；裁剪落在图像边界之外的部分；
- 按 DXGI 对应的 alpha、AND/XOR 规则合成，不把系统指针资源交给调用方。

只有鼠标位置、形状或可见性变化也算新的桌面状态，返回新的 `DesktopImage`。这样 Worker 缓存
的是已经合成光标的完整画面；后续 `DesktopNoChange` 可以安全复用，不会让光标停留在更早的
位置。关闭指针合成时跳过形状获取与混合，Synthetic 后端也不需要模拟 DXGI 指针 metadata。

## 5. 暂时不可用与致命错误

典型映射：

| 情况 | 分类 |
|---|---|
| `AcquireNextFrame` 超时 | `DesktopNoChange` |
| duplication access lost、设备重置或目标输出短暂不可用 | `DesktopTemporarilyUnavailable` |
| 下一次调用成功重建并取得图像 | `DesktopImage` |
| 首次 open 时目标输出不存在、格式不支持、参数错误或不变量破坏 | fatal error |
| staging 创建、复制或映射发生不可恢复错误 | fatal error |

检测到可恢复错误时，后端先释放所有已失效的 duplication、texture、context 和 pointer 状态，
标记需要重建，然后立即返回；下一次 `capture_latest()` 最多进行一次重建尝试。恢复期间暂时
找不到原输出仍然返回 `DesktopTemporarilyUnavailable`，由 Worker 的恢复时限判断它是否已经
成为会话级失败。后端不自行循环到恢复成功，避免占住 Capture Thread。

暂时不可用期间 Worker 可以继续按调度点复用最近有效画面，使短暂的锁屏、模式切换或 DXGI
重建不会立刻中断视频。Worker 必须另设连续失败次数或持续时间上限；超过上限后把会话标记为
失败，不能无限用陈旧画面伪装正常采集。该阈值在
[VideoCaptureWorker 设计](video-capture-worker.md) 中确定，不属于 Backend 配置。

后端错误必须保留发生操作和原生错误码。高频重试由统计计数，日志只记录首次进入恢复、恢复
成功和最终失败，避免每个调度点重复打印同一个 HRESULT。

## 6. 与 VideoCaptureWorker 协作

```text
FrameScheduler deadline
        |
        v
VideoCaptureWorker -- capture_latest() --> DesktopCaptureBackend
        |                    |
        |                    +-- DesktopImage: 替换最近画面
        |                    +-- NoChange: 保留最近画面
        |                    +-- Temporary: 保留画面并累计恢复状态
        |                    +-- Fatal: 会话失败
        v
CapturedVideoFrameStore
```

在当前 tick 上：

- 有 `DesktopImage` 时，Worker 将其像素存储移入新的共享不可变缓冲，并替换最近画面引用；
- NoChange 或暂时不可用且已有缓存时，Worker 从缓存构造重复帧；
- 尚无缓存时不生成空白帧；
- 每个实际发布帧获得连续的新 `sequence`、当前 tick 的 `presentation_time`，以及图像准备完成
  时的 `captured_at`；
- Store 满导致替换旧帧，与 Scheduler 晚到和 Backend 暂不可用分别统计。

后端永远不知道 `CapturedVideoFrameStore` 是否已满。Worker 向 Store 的 `try_push()` 仍然非阻塞，
下游过慢不能反向阻塞 DXGI 资源释放。

## 7. Synthetic 后端

`SyntheticDesktopCaptureBackend` 实现同一契约但不依赖 Windows、DXGI、GPU 或真实等待，用于
Windows/Linux CI 和后续无设备视频闭环。

它接受确定性的脚本或生成配置，可产生：

- 固定尺寸的纯色、色条、棋盘格或随调用编号变化的 BGRA 图像；
- NewImage 与 NoChange 的可控序列；
- 首图延迟、源分辨率变化和图像内容变化；
- 暂时不可用后恢复；
- 指定调用处的致命错误。

脚本耗尽后的默认行为是 `DesktopNoChange`，不隐式循环，也不读取系统时钟。图像内容应能从
像素和调用编号直接推导，方便验证画面复用、行布局、颜色转换和编码链路。Synthetic 后端
同样执行 open/capture/close 状态校验，使 Worker 测试不会绕过真实后端必须遵守的生命周期。

## 8. 统计

后端或 Worker 至少为采集阶段提供：

- 成功打开和关闭次数；
- 新桌面图像数与 NoChange 次数；
- 暂时不可用次数、恢复尝试和恢复成功次数；
- 致命采集错误数；
- 指针合成启用状态；
- 源尺寸和尺寸变化次数；
- CPU 图像复制平均、最大耗时和复制字节数。

Backend 可以返回完成统计所需的低频 metadata，但统计对象仍由 Worker 更新，避免平台后端
直接依赖 `PublisherStats`。

## 9. 测试

### 9.1 契约与无设备测试

- 非法输出选择、重复 open、未 open capture 和重复 close；
- DesktopImage 的紧凑 stride、大小、顶部起始布局和确定性像素；
- NewImage、NoChange、暂时不可用、恢复和 fatal 的完整结果序列；
- 首图之前 NoChange 不产生帧，首图之后 NoChange 复用最近画面；
- 源尺寸变化替换缓存，但不改变编码输出配置；
- 后端不等待真实时间，所有测试由调用次数推进；
- Worker 停止不依赖 Backend 返回新图像。

### 9.2 Windows DXGI 集成测试

- 默认主显示器和显式输出索引均可打开；
- 静止桌面允许返回 NoChange，画面变化后返回有效 BGRA；
- 分辨率、stride、颜色通道和画面方向可用截图或已知测试窗口核对；
- 开关指针合成时结果符合配置，三类可观测指针形状均不越界或崩溃；
- 模式切换、锁屏或 duplication 失效后能够恢复，或者在恢复上限后明确失败；
- 连续采集过程中 D3D debug layer 不报告资源泄漏或未配对 Map/ReleaseFrame；
- 1080p30 稳定性测试记录复制耗时和内存，不出现持续增长。

真实 DXGI 测试只在具备交互式 Windows 桌面的环境运行，不作为普通 CI 的硬性条件。CI 的
正确性基线由 Synthetic 后端和 Worker 契约测试提供。

## 10. 实施顺序

1. 定义平台无关契约与 `SyntheticDesktopCaptureBackend`，完成生命周期和结果语义测试；
2. 实现 `DxgiDesktopCaptureBackend` 的单输出、完整帧复制和无指针路径；
3. 加入旋转、尺寸变化和 access-lost 重建；
4. 实现并测试三类 DXGI 指针形状合成；
5. 按 [VideoCaptureWorker 设计](video-capture-worker.md) 实现调度、缓存和 FrameStore 接入；
6. 运行 Synthetic 与真实 DXGI 的 M1 编码链路验证。

## 11. 已决定

- 首版每个会话只采集一个输出，默认主显示器；
- Backend 只轮询最新桌面状态，不拥有线程、帧率等待、PTS 或重复帧策略；
- `AcquireNextFrame` 使用零超时，停止和 deadline 等待由 Worker 管理；
- Backend 返回独占、紧凑、顶部起始、正向的 CPU BGRA 图像，Worker 在跨线程发布边界将其
  移入共享不可变缓冲；
- DXGI GPU 资源不跨 Backend 边界；
- 完整帧复制优先，dirty/move rect 和 GPU 零拷贝延后到性能数据证明必要时；
- 暂时不可用与致命失败分开表达，Backend 不做无界重试；
- 恢复时绑定原输出，不静默切换显示器；
- 指针合成可配置，真实发布默认开启；
- Synthetic 后端不依赖设备和系统时钟，并可确定性注入所有结果类型。
