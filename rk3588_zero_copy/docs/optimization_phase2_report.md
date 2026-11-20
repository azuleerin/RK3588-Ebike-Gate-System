# optimization_phase2_report.md —— Phase 2 改造: Diff / 数值对拍 / 内存证据

> **分支**：`perf/hotpath-implementation-waste`(从 `main` @ `47a7ebb` 切出)
> **提交**：`f4f1838 perf(hotpath): 热循环实现层浪费改造`
> **日期**：2026-11-20
> **红线遵守**：① 只在独立分支改，没碰 main；② 每项改动都有 A/B；
> ③ 逐框/逐牌对拍（穷举，不是抽样）；④ 内存平线；⑤ Phase 3 恢复生产。

---

## 0. 改了什么（一览）

| 编号 | 改动 | 文件 | 净效果（实测） |
|---|---|---|---|
| **C2** | 识别输出**直读原生 fp16**（`want_float=0`），不再让驱动展成 float32 | `src/license_plate_rec.cc` | **−4.94 ms / 次识别** |
| **C3** | CTC 贪心解码改 **NEON 向量化** fp16 版 | `src/license_plate_rec.cc` | **−0.43 ~ −1.04 ms / 次识别** |
| **C6** | 检测 worker **取完输出立刻放核**，CPU 后处理挪到闸门外 | `src/rk_base/npu_pool.cc` | 每次检测少占核 **0.7 ms** |
| **C7/C8** | `publish_det` / `publish_ocr` 改 **move 语义** | `src/rk_base/npu_pool.cc` + `include/rk/npu_pool.h` | 每帧少一次**全局锁内**堆分配 |
| **C5** | 形态C 自检改**连续访问**（channel-outer） | `src/yolo_post.cc` | **−0.15 ms / 检测帧** |
| **C9** | `ocr_results_` map 取完结果**删除空壳条目**（修真实泄漏） | `src/rk_base/npu_pool.cc` | 活堆斜率 **+0.35 → 打平**（见 §4） |
| — | **不做**：检测侧 `want_float=0` | — | 实测净亏 0.9 ms/帧 |
| — | **不做**：`is_prealloc` | — | 2026-11-18 已完成 |

改动总量：5 个文件，+380 / −29 行。

---

## 1. C2 + C3：识别输出 fp16 直读 + NEON CTC

### 1.1 Diff（要点）

**`src/license_plate_rec.cc` —— init：把输出张量的原生类型查出来**

```cpp
    // ---- 4b) 输出张量的【原生类型】与量化参数 ----
    out_type_  = static_cast<int>(oa.type);
    out_qnt_   = (oa.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) && (oa.scale > 0.f);
    out_zp_    = out_qnt_ ? oa.zp : 0;
    out_scale_ = out_qnt_ ? oa.scale : 1.0f;
    size_t bpe = 0;
    switch (oa.type) {
        case RKNN_TENSOR_FLOAT32: bpe = 4; break;
        case RKNN_TENSOR_FLOAT16: bpe = 2; break;
        default: bpe = 0; break;      // int8/uint8/未知 -> 走 want_float=1 老路
    }
    out_native_ = (bpe != 0);
    out_bytes_  = out_native_ ? (size_t)out_T_ * out_C_ * bpe : 0;
```

**`src/license_plate_rec.cc` —— run_rknn：直读原生类型**

```cpp
    if (out_native_) {
        outputs[0].want_float = 0;      // 直读原生类型
        outputs[0].is_prealloc = 1;
        outputs[0].buf = out_raw_.data();
        outputs[0].size = static_cast<uint32_t>(out_raw_.size());
    } else {
        outputs[0].want_float = 1;      // 让驱动把量化输出反量化成 float32
        ...
    }
    ...
    if (out_native_ && out_type_ == RKNN_TENSOR_FLOAT16) {
        ok = ctc_greedy_decode_f16((const uint16_t*)outputs[0].buf, ...);
    } else if (out_native_) {
        ok = ctc_greedy_decode((const float*)outputs[0].buf, ...);
    } else { ... }
```

**`src/license_plate_rec.cc` —— 新增 NEON CTC 解码**

```cpp
#if defined(__aarch64__) || defined(__ARM_NEON)
            // ---- 全量模式: 6625 类 x 40 步 = 26.5 万次比较 ----
            float best = -3.0e38f;
            for (; c + 8 <= C; c += 8) {
                const float16x8_t h = vld1q_f16(reinterpret_cast<const __fp16*>(row + c));
                const float32x4_t lo = vcvt_f32_f16(vget_low_f16(h));
                const float32x4_t hi = vcvt_f32_f16(vget_high_f16(h));
                const float32x4_t mx = vmaxq_f32(vmaxq_f32(lo, hi), vdupq_n_f32(best));
                best = vmaxvq_f32(mx);
            }
            ...
            // 找【第一个】等于最大值的下标
            const uint32x4_t lane_idx = {0u, 1u, 2u, 3u};
            const uint32x4_t kNoHit = vdupq_n_u32(0xFFFFFFFFu);
            const uint32_t m_lo = vminvq_u32(vbslq_u32(vceqq_f32(lo, target), lane_idx, kNoHit));
            if (m_lo != 0xFFFFFFFFu) { found = c + (int)m_lo; break; }
```

**为什么这么改（改动原因）**：

1. PP-OCRv4 rec 的输出张量原生是 **fp16**（`[1,40,6625]`，265000 个元素）。
   `want_float=1` 让驱动把它展成 float32，**每次识别多写 530,000 字节**，
   实测这一步单独就要 **4.94 ms**（占整次识别 18%）。
2. CTC 贪心解码**真正用到的只有每个时间步的 argmax 和它的值** ——
   265000 个元素里只读了 **40 个**。整张 float32 张量是纯浪费。
3. fp16 → fp32 是 **IEEE 754 精确转换**（fp32 能无损表示任何 fp16 值），
   所以 argmax 下标与取值与 float32 版**逐位相同**。

**编译基线约束**：本工程编译基线是 ARMv8（无 FP16 算术扩展），
`vdupq_f16` / `vceqq_f16` 不可用。实现只用了基线就有的指令
（`vld1q_f16` / `vcvt_f32_f16` / `vmaxq_f32` / `vmaxvq_f32` /
`vceqq_f32` / `vbslq_u32` / `vminvq_u32`），**没有改全局编译选项**。

### 1.2 数值等价性（穷举证明，不是抽样）

**证明 ①：两条取数路径拿到的每一个元素都相同**

```
$ ./fp16_equiv --ocr models/ppocrv4_rec_fp16.rknn --imgs opt_audit/ocr_rois --iters 120
输出: type=1(FLOAT16) n_elems=265000 dims=[1,40,6625]

=== 识别输出逐元素穷举等价性 (120 帧) ===
  比对元素总数 : 31800000
  逐位不相等数 : 0
  最大绝对差    : 0
  结论          : ★ 完全等价(fp16->fp32 是精确转换, 一个 bit 都没差)

=== outputs_get 耗时对比 ===
  want_float=1 (驱动转 float32) : 5.171 ms
  want_float=0 (直读 fp16)      : 0.236 ms
  节省                            : 4.935 ms/次
```

> 31,800,000 个元素逐位（`memcmp`）比较，**0 差异**。
> 这不是抽样 —— 每一帧的每一个输出元素都比过了。
> 由于 fp16→fp32 是精确转换，**任何**下游解码器（CTC 贪心 / 白名单 CTC /
> softmax / top-k / 阈值判断）得到的输出必然逐位相同。

**证明 ②：两个解码器（float32 版 vs fp16 NEON 版）逐位一致**

```
$ ./ocr_decode_equiv --ocr models/ppocrv4_rec_fp16.rknn --dict models/ppocr_keys_v1.txt \
                      --imgs opt_audit/ocr_rois --iters 120

=== 合成压力测试 (4000 轮 x 40x6625 随机张量) ===
  [全量扫描] 文本不一致=0  分数逐位不一致=0
  [白名单]   文本不一致=0  分数逐位不一致=0
  结论: ★ 逐位一致

=== 生产解码实现等价性验证 (120 帧真实 ROI) ===
  [全量扫描模式] 文本不一致=0  分数逐位不一致=0  字符数不一致=0
  [白名单模式]   文本不一致=0  分数逐位不一致=0  字符数不一致=0
  结论: ★ 两个解码器逐位一致
  [耗时] float32 解码 = 0.831 ms/次 ; fp16 NEON 解码 = 0.400 ms/次 (省 0.431 ms)
```

合成压力测试故意灌了 6 类病态张量：全随机非负 / 全随机含负 / 大量并列最大值 /
全 0 / 全相同 / 含 ±Inf 与次正规数 —— 把边界全逼出来了。

> **这一步抓到过一个真 bug**：第一版 NEON 实现里，NaN 出现时 ARM 的 `FMAX`
> （`vmaxq_f32`）是 **NaN 传播**语义，而原版的 running max 是「严格大于才更新」
> —— NaN 会被**跳过**。两者语义不一致，所以第一版在随机张量上 **1334/4000 轮分数不一致**。
> 修法：检测到 NaN 就退回标量路径（`license_plate_rec.cc` 的 `std::isnan(best)` 分支）。
> **这是"逐框对拍不能只看抽样"的活例子** —— 真实 ROI 全是 softmax 概率，
> 永远不会有 NaN，抽样 120 帧 100% 通过，但随机压力测试把问题打了出来。

**证明 ③：端到端（生产类 `LicensePlateRec`）逐牌一致**

```
$ ./rec_base  --model models/ppocrv4_rec_fp16.rknn --dict models/ppocr_keys_v1.txt --row top    --image <217 张真实车牌>
$ ./rec_new   ...                                    --row top    --image <217 张真实车牌>
OCR row=top    : ★ 217 张真实车牌逐字节一致
OCR row=bottom : ★ 217 张真实车牌逐字节一致
```

217 张真实车牌里 **214 张有实际识别结果**（例：`"A359038" score=0.5443 字符数=7`），
两版输出**逐字节完全一致**（文本 + 分数 + 字符数）。

### 1.3 收益

| 口径 | 改前 | 改后 | 省 |
|---|---|---|---|
| `rknn_outputs_get`（每次识别） | 5.171 ms | 0.236 ms | **4.94 ms** |
| CTC 解码（每次识别，全量扫描） | 0.831 ms | 0.400 ms | **0.43 ms** |
| **一次识别推理合计** | **27.49 ms** | **22.51 ms** | **4.98 ms（18%）** |

> 两张真实车牌一次要提交 **4 个候选**（`gate_main.cc:1091-1103`：上行 3 个切分点 + 下行 1 个），
> 所以**每块牌省约 20 ms**。

---

## 2. C6：核闸门只包住真正的 NPU 段

### 2.1 Diff

```cpp
        rknn_run(s.ctx, nullptr);
        rknn_outputs_get(s.ctx, static_cast<uint32_t>(cfg_.det_n_output), outs, nullptr);
        const double dt_post0 = now_ms() - t0;

+       // ★★ 2026-11-20: 到这里就【提前放核】。
+       gate->release();
+
        // 后处理(在 worker 里做: 输出缓冲属于这个线程的上下文)
        ...
-       gate->release();
```

**改动原因**：生产配置是**每核 1 个检测 worker + 1 个识别 worker**
（`config/gate.env`：检测核 0,1,2 / 识别核 0,1,2 / 各 3 个 worker），
两个 worker 共用同一颗核的闸门。`rknn_run + rknn_outputs_get` 一返回，
NPU 核心就已经空闲，但闸门还被 CPU 后处理（实测 0.7 ms）占着 ——
同核的识别 worker 明明有空闲的核却只能干等，这段等待直接进 `ocrwait`。

**安全性论证**：
- 输出缓冲是 **slot 私有的**（`s.out_bufs[i]`），同核另一个 worker 用的是它自己 slot 的缓冲；
- 这个 slot 要到 `publish_det` **之后**才归还（`slot_m_` 保护），归还顺序没变；
- `rknn_run`/`rknn_outputs_get` 都已返回，NPU 不再读这块缓冲。

### 2.2 收益

Phase 3 实测（生产口径，4 路）：`ocrwait` ch1 **52.33 → 48.96 ms（−6.4%）**、
ch3 **57.54 → 53.67 ms（−6.7%）**，3 轮中位数一致。

---

## 3. C5 / C7 / C8

### 3.1 C5：形态C 自检改连续访问

```cpp
-        for (uint32_t a = 0; a < num_anchors; a++) {
-            for (int c = 0; c < nc; c++) { ... }
-            for (int c = 0; c < 4; c++)
-                box_max = std::max(box_max, box.at((uint32_t)c * num_anchors + a));
-        }
+        for (int c = 0; c < nc; c++) {
+            for (uint32_t a = 0; a < num_anchors; a++) { ... }
+        }
+        for (int c = 0; c < 4; c++) {
+            for (uint32_t a = 0; a < num_anchors; a++)
+                box_max = std::max(box_max, box.at((uint32_t)c * num_anchors + a));
+        }
```

**改动原因**：张量是 CHW 布局，同一通道的 8400 个 anchor 在内存里是连续的；
原写法每个 anchor 要跨 2+4=6 个相距 33 KB 的地址各取一个值 —— 6 条流、步长巨大，
硬件预取器跟不上。实测 **0.48 ms → 0.33 ms**（省 0.15 ms）。

**数值等价性论证**：求的是同一组元素的 max/min，与顺序无关。
唯一的顺序敏感点是 NaN（`std::min/max` 对 NaN 是"粘性"的）：
- `cls_max` 起始 `0.f`，NaN 操作数会被忽略 → 永远不会变成 NaN；
- `cls_min` 由 `first` 标志赋值，**两种写法的第一个元素都是 (a=0, c=0)**，
  而 `cls_min` 一旦变成 NaN 只可能来自这次初始赋值。

### 3.2 C7/C8：结果发布改 move

```cpp
-void NpuPool::publish_det(const DetResult& r) {
+void NpuPool::publish_det(DetResult&& r) {
-    det_results_[r.chan].push_back(r);
+    det_results_[r.chan].push_back(std::move(r));
```

**改动原因**：`DetResult` 带着 `std::vector<YoloBox> boxes`，
以前每帧要在**全局锁 `res_m_` 里面**做一次堆分配 + 拷贝框数组；
`OcrResult` 里的 `std::string text` 同理。4 路通道共用这把锁，
等于把一次堆分配塞进了所有通道的竞争路径。

---

## 4. C9：修一个真实的内存泄漏（`ocr_results_` map）

### 4.1 问题

`ocr_results_` 的键是 `(通道, group=track_id, 候选号)`，而 **track_id 一直递增**。
`publish_ocr` 往里插一条，`take_ocr` 取走结果后只 `pop_front()` ——
**空掉的 deque 还挂在 map 里，永不回收**。
每秒几十个识别候选 → 每秒几十条永不回收的 map 节点。

**实测证据（改造前，两臂都有）**：`mallinfo2().uordblks` 尾段斜率
**+0.33 ~ +0.35 KB/帧**（≈ +10 KB/s），项目自带的
`scripts/heap_slope.py` 判定为 **FAIL 泄漏 ~800 MB/24h**。

### 4.2 改法

```cpp
             *out = std::move(it->second.front());
             it->second.pop_front();
+            if (it->second.empty()) ocr_results_.erase(it);
```

**行为不变**：删的只是**已经空了**的条目；同一个键再来的结果
`publish_ocr` 会重新建一条，语义与以前完全一样。

### 4.3 效果

见 §6 与 Phase 3 报告的内存对比段。

---

## 5. 检测侧数值等价性（逐框对拍）

改造前后各编译一份 `hotpath_bench`（一个链接 `main` 的 `yolo_post.cc`，
一个链接改造后的），跑**同一批真实帧**（`output_debug/` 328 张真实帧取 120 张），
逐帧逐框输出 `cls / score / x1 / y1 / x2 / y2` 后**逐字节 diff**：

```
=== 逐框对拍 (120 帧真实图, 240 个推理结果, 2038 行) ===
★ 检测输出【逐字节完全一致】
```

另外 `hotpath_bench` 自带的交叉对拍（want_float=1 vs want_float=0，同一批帧）：

```
  >> 对拍 120 帧: 框数不一致帧=0  字段不一致=0  max|Δscore|=0.00000000  max|Δcoord|=0.00000000
```

---

## 6. 内存平线证据

见 Phase 3 报告 `optimization_ab_report.md` 的「内存」段
（3 分钟/轮 × 3 轮，工程自带 `scripts/heap_slope.py` 判定）。

---

## 7. 复现命令

```bash
# 基线二进制(main)
git worktree add /home/seeed/rk_deploy/opt_audit/main_wt main
cd /home/seeed/rk_deploy/opt_audit/main_wt/rk3588_zero_copy
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8 --target rk_gate

# 改造后二进制
cd /home/seeed/rk_deploy/rk3588_zero_copy
cmake --build build -j8

# 等价性验证
cd /home/seeed/rk_deploy/opt_audit/tools
./fp16_equiv        --ocr ../../models/ppocrv4_rec_fp16.rknn --imgs ../ocr_rois --iters 120
./ocr_decode_equiv  --ocr ../../models/ppocrv4_rec_fp16.rknn --dict ../../models/ppocr_keys_v1.txt \
                    --imgs ../ocr_rois --iters 120

# A/B
cd /home/seeed/rk_deploy/opt_audit && ./run_final_ab.sh
python3 ab_report.py 0      # 吞吐口径
python3 ab_report.py 20     # 生产口径
```

---

## 8. 本轮**没有**做的事（以及为什么）

| 没做 | 原因（实测） |
|---|---|
| 检测侧 `want_float=0`（切 int8 直读） | `outputs_get` 只省 **0.02 ms**，但 `OutTensor::at()` 的 int8 分支让后处理从 0.94 → 1.79 ms，**净亏 0.9 ms/帧** |
| 识别侧切 int8 | OCR 模型输出原生是 **fp16**，根本没有 int8 这条路 |
| `is_prealloc` 改造 | 2026-11-18 的 `8fdc9b7` 已经做完 |
| NMS 的 3 次 vector 分配 | 实测 **0.0002 ms**，低于测量噪声 |
| 跟踪器每帧 ~6 次 malloc | 每帧 ~2 µs 量级，不在 NPU 关键路径上；本轮不扩大改动面 |
| 识别 worker 也提前放核 | 需要把 `LicensePlateRec` 拆成「建张量 / 跑 NPU / 解码」三段，改动面大；
CTC 解码在 NEON 优化后只剩 0.4 ms，收益不足以支撑这次重构 |
