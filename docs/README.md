# 交付文档索引 · Documentation Index

本目录是 **RK3588 边缘 AI 大门感知系统** 的全部交付文档。正式交付件是
**[《RK3588 边缘 AI 系统交付白皮书》](RK3588边缘AI系统交付白皮书.pdf)**（PDF，含架构 / 实测数据 / 长跑证据 / 操作手册 / 排错 / 风险清单 / 交付清单）。

> 工程内更细的技术文档在 [`../rk3588_zero_copy/docs/`](../rk3588_zero_copy/docs/)，
> 精度回归在 [`../accuracy/`](../accuracy/)。

## 一、正式交付件

| 文档 | 用途 | 读者 |
|---|---|---|
| **[交付白皮书 PDF](RK3588边缘AI系统交付白皮书.pdf)** | **正式交付件**，可直接发给客户 / 老板 | 客户、领导 |
| [白皮书 Markdown 源](RK3588边缘AI系统交付白皮书.md) | 白皮书的可编辑源；改完用 `rk3588_zero_copy/scripts/md_to_pdf.sh` 重新出 PDF | 维护者 |
| [PPT 数据与素材包](PPT_ASSETS.md) | 做汇报直接抄的图表数据 + 截图清单 + 复现脚本 | 做 PPT 的人 |

## 二、压测与评估报告

| 文档 | 内容 |
|---|---|
| [12 路并发与硬件加速终极评估报告](FINAL_12CH_EVAL.md) | NPU 并发能力定频实测、ROI 分辨率仿真、端到端 MQTT 报文统计、逐目标精度回归计划 |
| [NPU 零拷贝 / 喂数参数 / 定频抖动体检报告](NPU_FREQ_LOCK_REPORT.md) | 定频前后延迟抖动对比（P99 −7.1%）、1/3/4/6 路吞吐实测、devfreq `load` 口径的坑 |
| [1080P 双实例并行提速验证](NPU_PIPE_1080P_REPORT.md) | `--npu-pipe 2` 在 1080P 下的提速幅度与 rktop 监控佐证 |
| [YOLOv8n INT8 640×640 性能排查与优化清单](YOLOV8_RK3588_PERF_CHECKLIST.md) | 从模型转换到推理参数的逐项排查清单 |
| [真实场景视频验收报告 (DEMO_VIDEO)](DEMO_VIDEO_REPORT.md) | 素材级回归：检测统计、车牌识别分布、内存与耗时 |
| [真实路况视频验收报告](REAL_ROAD_VIDEO_REPORT.md) | 实拍素材回归：车牌 ROI 几何问题、测速可用性、已知短板 |

## 三、稳定性与工程规约

| 文档 | 内容 |
|---|---|
| [崩溃调试规约](AGENT_RULES_FOR_CRASH_DEBUGGING.md) | **最高指令：稳定性压倒一切** —— 可二分、可回滚的干净基线，本项目所有稳定性决策的依据 |
| [最小化测试单元 · 架构改造方案](CRASH_DEBUG_MINIMAL_UNITS_PLAN.md) | 把大流水线拆成最小可运行单元的方案（用于硬挂死归因） |

## 四、素材资源

| 路径 | 内容 |
|---|---|
| [`assets/`](assets/) | PPT 图表（硬件加速公平 A/B/C 对比、像素保真度）与 Web 控制台截图 |

## 五、怎么重新生成交付 PDF

白皮书改完后重新出 PDF（需要板子上有 chromium）：

```bash
cd <仓库根目录>
bash rk3588_zero_copy/scripts/md_to_pdf.sh \
     "docs/RK3588边缘AI系统交付白皮书.pdf" \
     "docs/RK3588边缘AI系统交付白皮书.md"
```

> ⚠️ 图片用的是**相对路径**，所以要在**仓库根目录**执行本命令。
> 中间产物 `.html` 会留在 `docs/` 下，已在 `.gitignore` 中排除，不进版本库。
