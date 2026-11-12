# tools/pool —— dmapool（独立池化模块）

> **状态：独立待合入。** `rk12ch/` 仍在 QUARANTINE（根因未定位、单路侧未过 24h 回归），
> 冻结树一个字节都不许改，所以池化先在这里落地并自测，**解冻（step/08）时再接线**。
> 本模块**不依赖 rk12ch 的任何内部模块**，单元测试可以单独跑。

## 文件

| 文件 | 说明 |
|---|---|
| `dmapool.py` | 模块本体：`PooledBuffer` / `DmaBufPool` / `BufferLease` / `FrameSlotPool` / `pool_size()` / `build_pools()` |
| `test_dmapool.py` | 单元测试 11 项 |
| `test_lifecycle.py` | 流水线生命周期集成测试 4 项（自带最小队列/批次模拟） |
| `rk12ch_wiring.patch` | **解冻后**要打到 `rk12ch/` 的接线补丁（现在不生效，仅供 review） |

## 三个核心决策（已冻结）

### ① 按分辨率分池，绝不混用

MPP 解码输出 1080P NV12、RGA 输出 640×640 RGB、LPR 裁剪 160×48 —— 尺寸差异巨大，
三个独立池各自管理。混池会让借用逻辑背上"尺寸不匹配"，复杂度指数上升。

### ② 池大小 = 每路并发深度 × 路数 × 安全系数

```python
pool_size(per_channel_concurrency, channels, safety_factor, batch)
# = ceil(每路并发 × 路数 / batch) × 安全系数
```

12 路 / 每路并发 2 / 安全系数 1.5 的实测结果：

| 池 | 块数 | 块大小 | 合计 |
|---|---|---|---|
| `yolo-rgb`（RGA 输出） | 9 | (4×640+16)×640×3 = 4.72MB | **42.5MB** |
| `lpr-rgb`（车牌裁剪） | 5 | (8×48+16)×160×3 = 187KB | 0.9MB |
| `frame-nv12`（帧额度，不占内存） | 36 个 | — | 0 |
| **缓冲总内存** | | | **≈43.4MB** |

★ 池耗尽**绝不扩容**：`acquire()` 返回 `None` → 上层跳帧（丢帧优于吃内存）。池是内存上限的硬性锁死。

### ③ 用引用计数替代"用完就还"

一块 DMA-BUF 可能同时被 RGA 的输出和 NPU 的输入引用。RGA 写完就还池 → NPU 可能还在读 →
被下一帧覆盖。所以每块带引用计数：借出 +1，每个使用方 −1，**归零才回空闲链表**。

```python
lease2 = lease.retain()   # 转交第二个使用方 -> 返回【新的】lease（引用 +1）
lease.release()            # 第一个使用方用完 -> ref=1，不回池
lease2.release()           # 最后一个用完     -> ref=0，回池
```

> ★ 转交必须返回**新的** lease。若返回自己，两个使用方会抢同一个计数，引用数对不上 →
> 块提前回池 → NPU 还在读就被覆盖。

### fd / vaddr 终身不变

MPP 输出、RGA import、RKNN 绑定都只需要 fd；fd 在池初始化时确定后，每帧只**传同一个 fd**——
彻底消除"悬垂 fd / 重复 close / 忘记 close"这一类问题的土壤。
对比逐帧 new/delete DMA-BUF：必然在某条异常路径上泄漏 fd，跑几分钟后 dma-buf 耗尽 → 整条流水线哑掉。

## 三件套如何协同

| 件 | 作用 | 落地位置 |
|---|---|---|
| **模型权重共享** | 12 路共享同一份 YOLO/LPRNet 权重 | rk12ch 的 YOLO/LPRNet 各只有**一个 worker**，12 路在**同一个 context** 里组 batch 跑（分核 0_1 / 2），权重天然共享；每路只额外持有输入输出 buffer。若要"每路独立 context"，用 `rknn_dup_context`（权重共享、只多工作区，内存 ~300MB → ~100MB 量级） |
| **有界队列** | 队列里存 batch 指针 + 引用计数，长度固定；满了丢最旧并**归还块** | `pipeline/queues.py`：丢帧时调 `item.close()` → 归还帧引用 + 池化的块 |
| **DMA-BUF 池** | 所有缓冲来自池，物理内存总量启动时确定 | `dmapool.py` |

## 接线方案（解冻 step/08 时按 `rk12ch_wiring.patch` 打）

1. `rk12ch/rk12ch/hal/pool.py` ← 本模块（去掉 dma-heap 分配器，直接用 `rk12ch.hal.dmabuf`）
2. `rk12ch/rk12ch/app.py`：启动时 `build_pools(cfg)` 建三个池 → 传给预处理层 / 解码层 / 统计
3. `rk12ch/rk12ch/pipeline/preprocess.py`：`BatchBufferPool` → `DmaBufPool`；
   借块失败（池耗尽）→ 丢这一批并告警，**不扩容**
4. `rk12ch/rk12ch/pipeline/types.py`：`Batch.close()` 归还帧引用 + 池化块；`RefLease` 带帧额度
5. `rk12ch/rk12ch/pipeline/decode.py`：解码前取帧额度，借不到就跳过这一帧
6. `rk12ch/rk12ch/pipeline/queues.py`：丢最旧时 `item.close()`（归还块）
7. `rk12ch/config/system.yaml`：新增 `pool:` 段（`per_channel_concurrency` / `safety_factor` / `prefer_dmabuf`）
8. `rk12ch/rk12ch/stats.py`：统计表加"缓冲池"行（块数/在用/**峰值**/耗尽/重复归还）

> **峰值不随路数线性增长 = 池化生效**（内存曲线和单路一样平），这是验收判据。

## 跑测试

```bash
python3 tools/pool/test_dmapool.py     # 11 项
python3 tools/pool/test_lifecycle.py   # 4 项
```

## 环境限制（本板实测）

- `dma-heap` 存在但 **mmap 会 Permission denied**（任何用户）→ 池自动降级到 CPU 内存
  （`dmabuf=False`），RGA 零拷贝不可用，走 CPU 兜底。
- `heap_flags=RK_DMA_HEAP_UNCACHED` 在本板 **EINVAL**（不支持 uncached），
  分配器已按 `R+uncached → R+0 → H+0` 顺序重试。

## 为什么"MPP 解码输出"用的是额度池而不是真池

MPP 自己管理解码缓冲，Python 侧要真正接管分配得用 `MPP_DEC_SET_EXT_BUF_GROUP` +
`mpp_buffer_group`（C++ 侧才有完整 API，且当前板子的 VPU 驱动没加载，无法验证）。
所以这里用 `FrameSlotPool` 卡死**在途帧数**——它才是内存上限的决定因素：
借不到额度就跳过这一帧，而不是继续解、让 MPP 缓冲无限堆积。
真要接外部缓冲，在 C++ 侧用 `mpp_buffer_group_limit_set` + 池的 fd 即可，语义与本模块一致。
