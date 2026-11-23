# 00 · 任务书前提核查: ebike 的 ONNX/PT 源在本机**拿不到**

> 结论先行: 任务书写的「✅ ONNX 源在仓库:`02_Models_and_Data/training_data/detect_exp/
> exp02_person_ebike_gpu2/weights/` 下有 best.pt + best.onnx + best_split.onnx」
> ——**这个前提在本环境不成立**。那些路径属于**算法组的训练仓**(`E:\shenzhenproject`),
> 不在本仓库、不在本板、也不在 GitHub 上。
>
> 本仓库上一阶段(`docs/task2_fork9_ab.md` 第八节第 2 条)已经记过同一件事:
> 「**生产模型目前拿不到 fork9 版本**(ebike 的 ONNX 源不在仓库) —— 要出候选,
> 需要算法组补 ebike 的 ONNX(或 .pt)」。
>
> 本文件把这件事**查到底**, 并给出逐条可复现的证据。

---

## 一、七路排查(全部阴性)

| # | 途径 | 命令 / 做法 | 结果 |
|---|---|---|---|
| 1 | **全盘 `find`** 所有 `.onnx` / `.pt` | `find / -name "*.onnx" -o -name "*.pt"` | 只有 yolov8n / yolo26 / ppocrv4 的 ONNX;**没有任何 ebike 的 .pt/.onnx** |
| 2 | **目录结构** | `find / -type d -name "02_Models_and_Data" -o -name "01_Deliverable" -o -name "detect_exp"` | **0 命中** |
| 3 | **本仓库 git** | `git log --all --diff-filter=A --name-only \| grep -i "detect_exp\|02_Models\|ebike.*onnx"` | **0 命中**(这个仓从没有过训练数据) |
| 4 | **GitHub 上游** | `api.github.com/repos/azuleerin/RK3588-Ebike-Gate-System/git/trees/main?recursive=1` | 515 个条目, 与本地完全一致; 只有 `models/ebike_int8*.rknn`, **没有 ONNX** |
| 5 | **GitHub 账号下所有仓** | `api.github.com/users/azuleerin/repos` | **只有 1 个仓**(就是上面那个); release 里只有 `RK3588.AI.pdf` |
| 6 | **GitHub 全站搜仓名** | `search/repositories?q=02_Models_and_Data` / `exp02_person_ebike` / `Ebike+detect_exp` / `01_Deliverable+rknn` | 前者只命中无关仓, 后三个 **0 命中** |
| 7 | **内网可达性** | 扫 `192.168.8.0/24` 的 10/22/80/445/3389/8000/8080; 试 SSH | 板子已换网段(`192.168.31.77` → `192.168.8.77`), **原 Windows 工作站(`E:\shenzhenproject`)不可达**; 同网段两个 Ubuntu 主机 SSH 拒绝 |

补充:

* **归档里也没有**: 全盘 `.zip/.tar*/.7z/.rar`(>1MB)逐个查, 没有含 `detect_exp` 的包。
* **`.rknn` 容器里也没有 ONNX**: RKNN v2 是 **FlatBuffers** 容器(已解出 vtable 结构:
  `0a 00 10 00 04 00 08 00 0c 00` = vtable_len 10 / table_len 16 / 3 个字段偏移),
  里面存的是**编译后的算子表 + int8 权重**, 不是 ONNX proto:
  * 扫 `0x08 <ir_version> 0x3A` 的 ModelProto 头 → **0 命中**
  * 扫 zlib 头(`0x78 0x9c` 等)解压 → **0 命中**(没有压缩存放的原始模型)
  * 内嵌的**张量名**倒是完整保留(`/model.22/cv3.0/cv3.0.2/Conv`、
    `/model.22/dfl/Softmax` …), 所以**图结构可以还原**, 但**权重是量化后的 int8**。

---

## 二、那台 Windows 工作机在哪

`rk3588_zero_copy/docs/EBIKE_MODEL_RUNBOOK.md` 泄露了源头:

```
docker run --rm --mount "type=bind,source=E:/shenzhenproject,target=/work" --entrypoint bash rknn_env:2.5.0 -c '
cd /work && python3 tools/rknn/dump_dual_outputs.py \
  --onnx detect_exp/exp02_person_ebike_gpu2/weights/best_split.onnx \
  --calib calib/ebike_calib.txt ...'
```

⇒ `E:\shenzhenproject` 就是任务书里的 `01_Deliverable` + `02_Models_and_Data` 的宿主,
`best_split.onnx` 与 `calib/ebike_calib.txt` 都在那儿。
`~/.bash_history` 也有 `scp "E:\shenzhenproject\电动车素材.mp4" seeed@192.168.31.77:~/`
—— 板子曾经和那台机器同网段(`192.168.31.x`), 现在不在了。

**⇒ 需要算法组把下面两个文件传过来(或把整个 `02_Models_and_Data` 打包):**

```
detect_exp/exp02_person_ebike_gpu2/weights/best_split.onnx   ← 必需(首选)
detect_exp/exp02_person_ebike_gpu2/weights/best.pt            ← 备选(可重新导出)
calib/ebike_calib.txt                                         ← 建议(量化标定集)
```

---

## 三、为什么不能"就地还原"生产模型

生产模型 `models/ebike_int8_rk2332.rknn` 里**确实有** ebike 的权重(int8)。
理论上可以把它反解成 ONNX 再改头, 但这条路有三个硬伤:

1. **容器是私有 FlatBuffer**, 没有公开 schema。逐算子表能解析(名字+类型+耗时),
   权重区(名称→偏移→scale→int8 字节)的定位要靠"已知值模板反查"那一套,
   属于**逆向工程**,不是本任务该做的事。
2. **解出来的只是"量化后的权重"**, 重新量化会产生二次量化误差 ——
   那就不是"生产模型"了, 而是"生产模型的近似"。
3. 任务书要的候选模型, 应该是**算法组那份 `best_split.onnx` 的 fork9 版**,
   不是"从 .rknn 反推的版本"。**两者不可互换。**

⇒ 本阶段的做法: **把整条流水线(导出→转换→回归→A/B)全部打通并板端验证**,
候选模型用一个**结构与生产 ebike 完全一致**的 fork9 模型占位;
等 `best_split.onnx` 到位, **一条命令**就能换成真正的候选。

---

## 四、已就绪、接上 ONNX 就能跑的部分

| 产物 | 位置 | 状态 |
|---|---|---|
| fork9 导出(9 输出)脚本 | `scripts/build_ebike_arch_fork9.py` | ✅ 已跑通 |
| DFL 留图内(形态C)孪生脚本 | `scripts/make_dfl_in_npu_onnx_v3.py` | ✅ 已跑通(已修 opset 12 的 ReduceSum/Squeeze 轴形态) |
| INT8 转换脚本(toolkit 2.3.2) | `scripts/convert_rknn_int8.py` | ✅ 已跑通 |
| ebike 量化标定集 | `calib/ebike_calib.txt` (200 张) | ✅ 已造(替代算法组那份) |
| nc 自适应的回归解码器 | `src/acc_batch_nc.cc` | ✅ 已编译, 与原版**逐字节等价**(见 `docs/02_decoder_nc_fix.md`) |
| 一键出候选 | `scripts/build_ebike_fork9.sh` | ✅ 见该脚本注释 |

**接上真 ONNX 后的完整命令**(已写好, 直接用):

```bash
cd /home/seeed/rk_deploy/opt_ebike_fork9
cp <best_split.onnx> onnx/ebike_best_split.onnx

# 1) 原生 Detect -> fork9(9 输出, DFL 移出图)
python3 scripts/export_fork9_from_detect.py onnx/ebike_best_split.onnx onnx/ebike_fork9.onnx
# 2) 转 INT8 RKNN
python3 scripts/convert_rknn_int8.py onnx/ebike_fork9.onnx \
        models/ebike_fork9_int8.rknn --calib calib/ebike_calib.txt
# 3) 板端回归 + A/B
bash scripts/run_regression.sh
```
