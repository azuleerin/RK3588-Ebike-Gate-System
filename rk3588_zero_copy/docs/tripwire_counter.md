# 过线计数(二期 KPI)—— TripwireCounter

> 分支: `feature/gate-tripwire-counter`
> 状态: 已实现 + 单元测试(39 项断言全通) + 4 路集成实测

统计"从虚拟线上方驶入 / 驶出"的车辆数, 通过 MQTT 上报 JSON。

---

## 一、线程模型(红线: 完全异步, 绝不阻塞 NPU 主循环)

```
推理线程(Channel::run)              后台线程(TripwireService::worker_loop)
──────────────────────              ─────────────────────────────────────
submit()  ── 定长拷贝 ──┐          ┌──► 取快照 ──► 计数(叉积, O(1))
(不等待/不分配/不碰网络) └─────────►│              └► 组 JSON ─► MQTT
                                 └── 归还槽
```

- **推理线程只调 `submit()`**:一次定长数组拷贝 + 一次微秒级互斥量就返回。
  **不等待结果、不碰 socket、不分配堆内存。**
- 计数、JSON 组包、MQTT 发送、连接重连**全在后台线程**。
- 帧快照走**定长环形缓冲**;队列满了**丢最旧的一帧**并如实计数(不静默丢),
  绝不因为业务慢而卡住 NPU。

**实测(4 路并发, 60 s)**:

| | ch1 | ch2 | ch3 | ch4 |
|---|---|---|---|---|
| 不开过线计数 | 16.8 fps | 20.0 fps | 16.4 fps | 17.8 fps |
| 开过线计数 | — | 20.0 fps | 17.1 fps | 18.0 fps |

帧率与 detwait 均无劣化 —— 计数完全在主循环之外。

---

## 二、几何与方向(全部 O(1))

用**向量叉积**, 不做任何轮询:

```
side(P) = (x2-x1)*(P.y-y1) - (y2-y1)*(P.x-x1)
```

- `side > 0` → P 在有向直线 `P1->P2` 的正侧; `< 0` → 负侧; `= 0` → 在线��。
- **穿越判定**: `previous_center -> current_center` 这条线段与虚拟线段**真正相交**
  (4 个叉积 + 符号判断, 含共线重叠 / 端点相碰的退化情形)。
  → 只穿过**延长线**不算数。
- **方向**: 负侧 → 正侧 = 正方向穿越; 反之负方向。
  `positive_is_in=true` 时正方向算"进"。

**直觉对照**: 线**从左画到右**(`x1<x2, y1==y2`)时 `side(P) = (x2-x1)*(P.y-y1)`,
线下方为正、上方为负, 于是 `positive_is_in=true` 就是 **"从上到下为进, 从下到上为出"**。

---

## 三、生命周期(严格防泄漏)

- 每个 `(camera_id, track_id)` 在 `std::unordered_map` 里存一份历史:
  上一次中心点 + 上一次所在的**非零**侧 + 最后出现的帧号。
- **连续 `max_miss_frames` 帧(默认 30)没更新的轨迹会被清掉**, 每
  `cleanup_period_frames` 帧(默认 10)扫一次 —— 摊销成 O(在册数/周期)。
- 另有 `max_tracks`(默认 512)硬上限兜底; 满了就拒绝新轨迹并计数上报。
- 地图在构造时 `reserve`, 运行期只增删不扩容。

---

## 四、误判防护

| 现象 | 处理 |
|---|---|
| 目标站着不动 | 位移 < `min_move_px`(默认 2 px)不判定 |
| `track_id` 被复用(换了个目标) | 一帧位移 > `max_jump_px`(默认 400 px)→ 重置历史, **不计数** |
| 目标正好停在线上 | 保留上一个**非零**侧, 等它离开线的那一帧再判定 —— 不漏计也不重复计 |
| 只想统计某些类别 | `--tripwire-classes 0,1` |

---

## 五、用法

```bash
# 虚拟线(原图像素坐标): 从左到右画一条 y=400 的线 -> "从上到下为进"
./build/rk_gate ... --tripwire-line 100,400,1180,400

# 反过来: "从下到上为进"
./build/rk_gate ... --tripwire-line 100,400,1180,400 --tripwire-in negative

# 只统计某些类别 / 调整超时 / 只本地计数不发 MQTT
--tripwire-classes 0,1 --tripwire-miss-frames 30 --tripwire-no-mqtt
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--tripwire-line x1,y1,x2,y2` | 无 | 给了才启用过线计数(原图像素坐标) |
| `--tripwire-in positive\|negative` | `positive` | 哪个方向算"进" |
| `--tripwire-classes 0,1` | 全部 | 只统计这些类别 |
| `--tripwire-miss-frames N` | 30 | 轨迹多少帧没更新就清掉 |
| `--tripwire-interval-ms N` | 1000 | 统计上报周期 |
| `--tripwire-topic <topic>` | `device/<sn>/ch<N>/message/up/tripwire` | 覆盖上报 topic |
| `--tripwire-no-mqtt` | 关 | 只本地计数, 不发 MQTT |

---

## 六、上报报文

Topic: `device/<sn>/ch<N>/message/up/tripwire`(跟着现有的通道 topic 走)

```json
{"type":"tripwire","sn":"rk3588-01","ch":1,"camera_id":0,
 "line":{"x1":100.0,"y1":400.0,"x2":1180.0,"y2":400.0},
 "in":137,"out":135,"total":272,"active_tracks":1,"dropped_frames":0,
 "ts":1763345000123}
```

| 字段 | 含义 |
|---|---|
| `in` / `out` | 累计进 / 出的人数 |
| `total` | `in + out` |
| `active_tracks` | 当前在册(缓存里)的轨迹数 |
| `dropped_frames` | 因队列满被丢掉的帧 —— 业务质量指标, 不静默丢 |

---

## 七、单元测试

```bash
cmake --build build --target test_tripwire -j8
./build/test_tripwire
```

独立的测试主函数, **不依赖录像/硬件**, 全部用伪造轨迹驱动。覆盖 39 项断言:

| 组 | 内容 |
|---|---|
| ① 几何 | 叉积侧别; 线段相交(十字/平行/延长线/端点相碰/共线重叠/分离) |
| ② 方向 | 上→下=进, 下→上=出, `positive_is_in=false` 反转, 来回穿 |
| ③ 边界 | 只穿延长线不计数 / 线段外不计数 / 斜线正确计数 |
| ④ 抖动与瞬移 | 原地不动不计数, 1px 微抖动不计数, 瞬移 500px 不计数 |
| ⑤ 生命周期 | 超时 10 帧(阈值 5)→ 全部清掉; 持续更新的轨迹不被误清 |
| ⑥ 多目标与类别 | 多条轨迹互不干扰; 类别过滤生效 |
| ⑦ `vector<TrackResult>` 入口 | 主工程实际走的那条路径 |
| ⑧ 场景 | 两辆车分别从上往下 / 从下往上穿过 |
| ⑨ 异步服务 | 200 次 submit 总耗时 < 50 ms; 后台计数准确; 队列满时如实报丢帧; JSON 字段完整 |
| ⑩ 并发提交 | 4 线程各提交 250 帧, 不崩, 丢帧如实计数 |

---

## 八、代码位置

| 文件 | 内容 |
|---|---|
| `include/rk/tripwire.h` | `TripwireConfig` / `TripwireTrack` / `TripwireEvent` / `TripwireStats` / `TripwireCounter` / `TripwireService` |
| `src/rk_base/tripwire.cc` | 叉积几何 + 计数 + 清理 + 异步服务 + JSON/MQTT |
| `tests/test_tripwire.cpp` | 独立测试主函数 |
| `src/gate_main.cc` | 接入: CLI + 每帧 `submit()` + 收尾 `stop()` |
| `include/speed_track.h` | `TrackResult` 补了 `center_x/center_y`(框中心) |

---

## 九、已知边界

- 坐标系是**原图像素**。不同分辨率的相机要各画各的线(每路一个服务, 互不影响)。
- 计数精度取决于 ByteTrack 的轨迹质量: 跟踪 ID 跳变(丢目标后重新分配)会产生一条
  新轨迹, 那一次穿越会漏计 —— 这是上游跟踪的问题, 不是计数器的。
- 单帧最多带 128 条轨迹(超出部分被截断并计数上报); 实测路口场景远达不到。
- 极短时间内同一 `track_id` 反复穿过线会反复计数 —— 真实场景里车辆不会这样,
  但如果需要"一辆车只计一次", 得再加一个"已计过的 track_id 集合"(当前没做, 因为
  需求没提, 而且那个集合的 TTL 策略需要业务确认)。
