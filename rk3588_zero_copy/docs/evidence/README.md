# 极限吞吐压测 —— 证据包

**测试时间**：2026-11-19 17:49 ~ 17:56
**板子**：RK3588-GATE-01（recomputer-rk3588-devkit）
**配置**：`--max-fps 0`（解除限帧）· det 核 0,1,2 · ocr 核 0,1,2 · det-slots 2 · `--det-workers 3 --ocr-workers 3`
**素材**：4 路本地视频（ch1 30fps / ch2 21.5fps / ch3 60fps / ch4 60fps，合计源上限 171.5 FPS）

---

## 一句话结论

| | 4 路合计交付 FPS | 说明 |
|---|---|---|
| `--max-fps 20`（原生产配置） | 79.20 | 4×20=80，打到源上限的 99% —— **不是算力极限** |
| **`--max-fps 0`（解除限帧）** | **95.20** | **真实算力上限，+20.2%** |
| 源视频合计上限 | 171.5 | 说明 95.2 不是被素材限制的 |

**瓶颈 = NPU 推理吞吐**（不是解码 / RGA / CPU / 帧率上限）。

---

## 文件清单

| 文件 | 用途 |
|---|---|
| `throughput_panel.png` | **PPT 主图**：吞吐对比 + 瓶颈定位 + 否定实验，一张图讲完 |
| `rknpu_load.png` | NPU 三核负载图（`/sys/kernel/debug/rknpu/load` 实采） |
| `htop_cpu.png` | htop 满载实况截图（tmux 抓屏 → ANSI → PNG），可见 rk_gate 的 9 个线程各占 6~9% CPU |
| `gate_log_1min.txt` | `gate.log` 精确 60 秒切片（17:53:59 → 17:54:59，59 行），含四路统计行与车牌识别行 |
| `perf_sampling.txt` | 3 分钟原始采样表（12 次 × 4 路）+ 字段释义 + 统计 |
| `rknpu_load.txt` | NPU 三核原始文本采样（5 次连续） |
| `htop_cpu.ansi` / `*.ansi` | PNG 的 ANSI 源文件（便于重新渲染或改文案） |

---

## 瓶颈定位：怎么判读

`gate_main.cc:1240` 自带的判读法：

> `det_wait`：等检测结果的墙钟（含 NPU 排队）—— 比"推理本身"长，说明 NPU 不够用；
> 跟"推理本身"差不多，说明慢在别处（解码/RGA/后处理）。

| 指标 | 实测 | 判读 |
|---|---|---|
| `npu`（纯推理耗时） | 26.0 ms | 单帧 NPU 推理 |
| **`detwait`（含排队）** | **33 ~ 38 ms** | **比 npu 多 9 ms → NPU 排队** |
| `ocrwait` | 0（ch2/ch4 无目标）/ 54~88 ms（ch1/ch3 检出文字时） | 仅 ch1/ch3 触发识别 |
| NPU 三核 | 70 ~ 83% | 算力接近饱和 |
| 整机 CPU | 12 ~ 20% | **不是瓶颈** |
| `rga`（NV12→RGB） | 2.0 ~ 7.1 ms | **不是瓶颈** |

## 否定实验：det_slots 不是杠杆

| 配置 | 合计 FPS | detwait |
|---|---|---|
| `det_slots=2`（原配置） | **95.4** | 34.5 ms |
| `det_slots=4`（4 槽/worker × 3 worker = 12 并发） | 93 ~ 99 | **36.5 ~ 38.6 ms** |

提高 NPU 并发槽位**反而更差**（排队时间变长，吞吐没上去）。
**结论：并发槽位不是瓶颈，NPU 算力本身是墙** —— 想再往上走只能换更快的 NPU 模型或减少每帧计算量。

---

## 复现方法

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
sudo vi config/gate.env
#    GATE_MAX_FPS=0        # 解除限帧
sudo systemctl restart rk-gate
sleep 20                                   # 预热
grep -a '\[ch[0-9]\]\[frame' /var/log/rk-gate-error.log | tail -4   # 看四路 deliv
sudo cat /sys/kernel/debug/rknpu/load      # 看三核

# 抓 htop 截图
tmux new-session -d -x 130 -y 42 -s htop 'htop -d 3'
tmux capture-pane -t htop -e -p > htop.ansi
python3 scripts/ansi_to_png.py htop.ansi htop.png "标题"

# 压测完务必恢复
sudo vi config/gate.env && sudo systemctl restart rk-gate   # GATE_MAX_FPS=20
```

> ⚠️ 压测期间整机 CPU 会升到 12~20%、NPU 三核 70~83%，属正常现象；
> 测完**必须把 `GATE_MAX_FPS` 改回 20** 并重启。
