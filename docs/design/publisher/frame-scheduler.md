# FrameScheduler 设计

本文定义 Publisher 视频轨道的固定帧率调度规则，是
[Publisher 音视频设计总览](overview.md) 中视频时间模型的详细设计。首版只配置 30 fps，
接口使用有理帧率表示，避免把 `29.97` 等帧率编码为浮点数或提前封死为整数帧率。

## 1. 目标与边界

`FrameScheduler` 负责：

- 根据视频轨道启动时刻和帧率生成绝对单调时钟 deadline；
- 为每个调度点生成相对共享会话原点的 `presentation_time`；
- Worker 处理过慢时跳过已经过期的调度点；
- 返回本次推进跳过的调度点数量，供统计使用；
- 使用基于帧索引的整数运算，避免逐帧累加截断误差。

`FrameScheduler` 不负责：

- 创建线程或执行 `sleep_for()`、`sleep_until()`；
- 处理 `stop_token`、条件变量或 Worker 控制命令；
- 调用 DXGI、判断桌面是否变化或缓存 BGRA 图像；
- 分配视频帧序号；
- 根据采集完成时间重写 PTS；
- 换算编码器或 RTP 的 90 kHz 时间戳。

Scheduler 只由 Video Capture Thread 使用，不提供内部同步，也不支持跨线程并发调用。每次
发布会话为已启用的视频轨道创建一个新实例；停止后不复用旧实例。

## 2. 时间语义

### 2.1 三类时间

| 名称 | 含义 |
|---|---|
| `session_origin` | 整个发布会话共享的 `steady_clock` 原点 |
| `track_start` | 视频采集后端打开且 Capture Worker 准备就绪的时刻 |
| `deadline` | 某个视频调度点应开始获取最新桌面画面的绝对单调时刻 |

`track_start` 不得早于 `session_origin`。视频轨道可能晚于会话原点启动，因此首帧媒体时间
不要求为零：

```text
first_presentation_time = track_start - session_origin
```

这使首帧 PTS 表达真实的视频轨道起始位置，后续加入音频时不需要把两条轨道伪装成同时
产生首个媒体单元。

### 2.2 调度点

第 `n` 个调度点从零开始编号：

```text
frame_offset(n) = round(n * 1 second * rate_denominator / rate_numerator)
deadline(n) = track_start + frame_offset(n)
presentation_time(n) = deadline(n) - session_origin
```

所有运算使用检查过的整数算术并按最近纳秒舍入，不使用浮点数。每个 offset 都从
`track_start` 和完整帧索引计算，禁止把截断后的单帧周期反复累加。

对于 30/1 fps：

```text
tick 0: 0 ns
tick 1: 33,333,333 ns
tick 2: 66,666,667 ns
tick 3: 100,000,000 ns
...
tick 30: 1,000,000,000 ns
```

因此运行时间增长不会积累 `1 second / 30` 的纳秒截断误差。

## 3. 领域接口草案

```cpp
struct FrameRate {
    std::uint32_t numerator = 30;
    std::uint32_t denominator = 1;
};

struct FrameTick {
    std::uint64_t schedule_index = 0;
    std::chrono::steady_clock::time_point deadline{};
    MediaTime presentation_time{};
};

struct FrameSchedule {
    FrameTick tick;
    std::uint64_t skipped_ticks = 0;
};

class FrameScheduler final {
public:
    using Clock = std::chrono::steady_clock;

    FrameScheduler(SessionTimeline timeline,
                   FrameRate frame_rate,
                   Clock::time_point track_start);

    [[nodiscard]] FrameTick initial_tick() const;
    [[nodiscard]] FrameSchedule advance_after(Clock::time_point now);
};
```

这是实现边界而不是必须逐字保持的最终声明。最终 API 可以调整命名，但必须保留以下语义：

- 构造后首个调度点固定为 `track_start`；
- `initial_tick()` 不改变调度状态；
- Worker 每处理完一个已取得的 tick，只调用一次 `advance_after(now)`；
- `advance_after()` 返回严格晚于上一个 tick、且 deadline 不早于 `now` 的第一个调度点；
- `skipped_ticks` 是两个已返回 tick 之间没有交给 Worker 处理的调度点数量。

`FrameScheduler` 可以按值保存不可变的 `SessionTimeline`，避免保存对外部对象的悬空引用。

## 4. 晚到和跳帧策略

Worker 处理完当前调度点后，将新的 `steady_clock::now()` 传给 Scheduler。Scheduler 先计算
下一个索引；如果其 deadline 已早于 `now`，继续向后查找，直到找到 deadline 等于或晚于
`now` 的调度点。

例如 30 fps 轨道：

```text
当前已处理 tick: 3，deadline = 100.000 ms
Worker 完成时间:             180.000 ms
tick 4 deadline:             133.333 ms  -> 跳过
tick 5 deadline:             166.667 ms  -> 跳过
tick 6 deadline:             200.000 ms  -> 返回
skipped_ticks:               2
```

不得立即补采 tick 4 和 tick 5。突发补帧不能恢复已经错过的桌面状态，只会增加编码和发送
积压。返回 tick 6 后，Worker 可停止地等待到 200 ms。

deadline 恰好等于 `now` 时，该 tick 不算过期，Worker 可以立即处理。PTS 使用被返回 tick 的
`presentation_time`，所以跳过调度点会形成真实的媒体时间缺口，不通过压缩后续 PTS 隐藏
过载。

## 5. 与 VideoCaptureWorker 协作

推荐循环：

```text
1. 打开 DesktopCaptureBackend
2. 记录 track_start = steady_clock::now()
3. 创建 FrameScheduler 并取得 initial_tick
4. 可停止地等待到 tick.deadline
5. 请求后端提供最新桌面图像
6. 有新图像时更新 Worker 私有的最近画面
7. 有最近画面时构造 CapturedVideoFrame 并发布
8. 使用处理完成后的 now 调用 advance_after
9. 累加 skipped_ticks，回到步骤 4
```

等待由 `VideoCaptureWorker` 的条件变量和停止状态完成。伪唤醒、资源 hint 或控制命令唤醒后，
Worker 必须重新检查状态和 deadline；Scheduler 不感知这些同步细节。

### 5.1 桌面无变化

每个调度点代表“输出一帧”，不代表 DXGI 必须报告桌面发生变化：

- 有新图像时，缓存并输出新图像；
- 没有新图像但已有缓存时，复制或引用最近的有效图像内容并输出重复帧；
- 尚未取得任何有效图像时，不生成空白帧，本调度点不产生 `CapturedVideoFrame`；
- 重复帧具有新的 `sequence` 和当前 tick 的 `presentation_time`。

`CapturedVideoFrame::sequence` 只统计实际发布到 FrameStore 的帧，从零连续递增，不等同于
`schedule_index`。跳帧和首帧前无图像通过统计与 PTS 缺口体现，不把调度索引塞入媒体帧。

### 5.2 PTS 与采集时间

```text
frame.presentation_time = tick.presentation_time
frame.captured_at = 实际完成本次采集或画面准备的 steady_clock::now()
```

不得使用 `captured_at - session_origin` 覆盖 `presentation_time`。前者用于采集、编码和发送
阶段延迟统计，后者用于媒体呈现和 RTP 时间戳映射。

### 5.3 后端结果

后端至少需要区分：

- 取得新图像；
- 本次没有新图像；
- 可恢复的暂时状态；
- 不可恢复错误。

画面结果和错误分类由 [DesktopCaptureBackend 设计](desktop-capture-backend.md) 定义。
Scheduler 无论采集成功与否都只处理调度点；不可恢复错误由 Worker 上报并使发布会话失败。

## 6. 校验与溢出

构造时必须拒绝：

- 分子或分母为零的帧率；
- `track_start` 早于 `session_origin`；
- 无法在纳秒精度下产生严格递增 deadline 的帧率；
- 初始时间计算超出 `steady_clock::time_point` 或 `MediaTime` 可表示范围。

推进时必须检查帧索引、整数乘法、时间偏移和 deadline 加法溢出。配置错误使用明确异常在
Worker 启动阶段失败；运行到不可表示的极端时长属于致命内部错误，不允许无符号回绕后继续
发布。

## 7. 统计

Scheduler 直接提供 `skipped_ticks`，Worker 负责累计和对外暴露：

- 已处理的调度点数；
- 因 Worker 晚到跳过的调度点数；
- 首个有效桌面图像前未输出的调度点数；
- 使用新桌面图像产生的帧数；
- 复用最近画面产生的帧数；
- 发布到 FrameStore 的帧数；
- FrameStore 满时替换的帧数。

“调度晚到跳帧”和“FrameStore 替换旧帧”必须分别统计：前者发生在采集之前，后者发生在
采集之后，二者对应不同的性能瓶颈。

## 8. 测试

FrameScheduler 单元测试不启动线程、不真实等待，直接传入构造出的 `steady_clock::time_point`：

- 30/1 fps 的前几个 deadline 和 PTS 使用正确舍入；
- 第 30 个 offset 精确为 1 秒，不累计纳秒截断误差；
- 30000/1001 等有理帧率不使用浮点数且长期保持公式一致；
- `track_start` 晚于 `session_origin` 时首帧 PTS 保留真实偏移；
- 正常推进返回相邻调度点且 `skipped_ticks == 0`；
- `now` 恰好等于下一 deadline 时不跳帧；
- `now` 晚于多个 deadline 时返回首个未过期 tick 和准确跳过数量；
- 模拟长时间运行，结果始终等于从索引直接计算的时间；
- 非法帧率、负的轨道起始媒体时间和所有可构造的溢出路径被拒绝。

VideoCaptureWorker 测试另外使用 Fake Scheduler Clock 或显式注入的等待边界，验证停止能够
立即唤醒、无新桌面图像时复用缓存、首帧前不输出空白以及统计分类。Scheduler 单元测试不
承担线程停止测试。

## 9. 已决定

- 首版配置 30/1 fps，领域接口使用有理帧率；
- 第一帧 deadline 等于视频轨道实际启动时刻；
- PTS 来自计划调度点，不来自采集完成时间；
- deadline 使用绝对时刻和帧索引计算，不采用相对 `sleep_for()`；
- Worker 晚到时跳过过期调度点，不突发补帧；
- Scheduler 是 Capture Thread 私有的纯调度对象，不拥有等待和停止机制；
- Scheduler 不判断桌面变化，画面复用属于 VideoCaptureWorker 职责；
- 每次发布会话创建新 Scheduler，不支持跨会话 reset。
