#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_pruned_models.py —— 3 档剪枝模型 -> ONNX -> RKNN INT8

流程:
  源 ONNX (ReLU 512 / SiLU 576)
      -> prune_onnx.py 结构化通道剪枝 (20% / 30% / 40%)
      -> 校验 (结构自洽 + 输出形状不变)
      -> RKNN Toolkit 2.3.2 量化导出 (w8a8 / per-tensor / optimization_level=3)
      -> 板端核对输出形状

★ 量化口径与第二步完全一致 (同一份校准集 / 同一套 config), 保证组间可比。

用法:
    ./build_pruned_models.py [--families relu512,silu576] [--ratios 0.2,0.3,0.4]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "model_opt"))

from export_quantize import build_rknn, verify_output_shapes   # noqa: E402

# family: (源 ONNX 相对路径, 输入尺寸, 家族名, 说明)
FAMILIES = {
    "relu512": (os.path.join(REPO, "model_opt", "onnx", "yolov8n_relu_512.onnx"),
                512, "ReLU", "第二步性能最优(但未微调时检不出东西)"),
    "silu576": (os.path.join(REPO, "model_opt", "onnx", "yolov8n_silu_576.onnx"),
                576, "SiLU", "第二步推荐的可落地基线"),
}
RATIOS = [0.2, 0.3, 0.4]
CALIB = os.path.join(REPO, "_bench", "calib_list.txt")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--families", default="relu512,silu576")
    ap.add_argument("--ratios", default="0.2,0.3,0.4")
    ap.add_argument("--out-dir", default=os.path.join(HERE, "models"))
    ap.add_argument("--onnx-dir", default=os.path.join(HERE, "onnx"))
    ap.add_argument("--calib", default=CALIB)
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()

    families = [f.strip() for f in args.families.split(",") if f.strip()]
    ratios = [float(r) for r in args.ratios.split(",") if r.strip()]
    os.makedirs(args.out_dir, exist_ok=True)
    os.makedirs(args.onnx_dir, exist_ok=True)

    summary = {"calib": args.calib, "models": {}}

    for fam in families:
        src, size, act, desc = FAMILIES[fam]
        if not os.path.exists(src):
            print("跳过 %s: 源 ONNX 不存在 %s" % (fam, src))
            continue
        print("\n" + "=" * 78)
        print("家族 %s  (%s, %d×%d)  —— %s" % (fam, act, size, size, desc))
        print("=" * 78)
        for ratio in ratios:
            tag = "%s_p%02d" % (fam, int(ratio * 100))
            pruned_onnx = os.path.join(args.onnx_dir, "yolov8n_%s_p%02d.onnx"
                                       % (act.lower(), int(ratio * 100)))
            rknn_path = os.path.join(args.out_dir, "%s.rknn" % tag)

            print("\n  === %s  剪枝 %.0f%% ===" % (tag, ratio * 100))
            t0 = time.time()
            if not (args.skip_build and os.path.exists(pruned_onnx)):
                r = subprocess.run(
                    [sys.executable, os.path.join(HERE, "prune_onnx.py"),
                     "--onnx", src, "--out", pruned_onnx,
                     "--ratio", str(ratio),
                     "--report", os.path.join(args.out_dir, "%s_prune.json" % tag)],
                    capture_output=True, text=True)
                print(r.stdout.strip())
                if r.returncode != 0:
                    print(r.stderr[-2000:])
                    raise SystemExit("剪枝失败: %s" % tag)

            if not (args.skip_build and os.path.exists(rknn_path)):
                # ★ rknn-toolkit2 会把中间优化产物(check*_*.onnx)写到【当前工作目录】,
                #   所以构建期间临时切到临时目录, 免得污染交付目录。
                import tempfile
                cwd = os.getcwd()
                tmpd = tempfile.mkdtemp(prefix="rknn_build_")
                try:
                    os.chdir(tmpd)
                    info = build_rknn(pruned_onnx, rknn_path, size, args.calib,
                                      per_tensor=True)
                finally:
                    os.chdir(cwd)
                    shutil.rmtree(tmpd, ignore_errors=True)
            else:
                info = {"rknn": rknn_path,
                        "size_mb": os.path.getsize(rknn_path) / 1048576.0}

            sh = verify_output_shapes(rknn_path, size)
            print("    输入形状 %s | 输出 %d 个 | 形状核对 %s"
                  % (sh["input_dims"], sh["n_output"],
                     "OK" if sh["input_ok"] else "★不一致★"))
            pr = json.load(open(os.path.join(args.out_dir, "%s_prune.json" % tag)))
            summary["models"][tag] = {
                "family": fam, "ratio": ratio, "activation": act, "size": size,
                "onnx": pruned_onnx, "rknn": rknn_path,
                "rknn_mb": info["size_mb"], "shape_ok": sh["input_ok"],
                "input_dims": sh["input_dims"], "outputs": sh["outputs"],
                "params_before": pr["params_before"], "params_after": pr["params_after"],
                "param_reduction_pct": pr["param_reduction_pct"],
                "build_sec": round(time.time() - t0, 1),
            }

    with open(os.path.join(args.out_dir, "pruned_models.json"), "w") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False)

    print("\n" + "=" * 78)
    print("%-14s %-10s %-12s %-12s %-10s %-10s" %
          ("模型", "比例", "参数量(M)", "参数减少", "RKNN(MB)", "形状"))
    for tag, m in summary["models"].items():
        print("%-14s %-10.0f%% %-12.3f %-12.2f%% %-10.2f %-10s" %
              (tag, m["ratio"] * 100, m["params_after"] / 1e6,
               m["param_reduction_pct"], m["rknn_mb"],
               "OK" if m["shape_ok"] else "★异常★"))
    print("=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
