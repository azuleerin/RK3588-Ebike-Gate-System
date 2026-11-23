#!/usr/bin/env bash
# ============================================================================
# build_ebike_fork9.sh —— 一条命令: ebike 的 ONNX -> fork9 ONNX -> INT8 RKNN
#
# ★ 用法
#     ./scripts/build_ebike_fork9.sh <源 ONNX> [输出前缀]
#
#   例:
#     ./scripts/build_ebike_fork9.sh onnx/ebike_best_split.onnx
#
# ★ 源 ONNX 是什么
#   瑞芯微 fork 的 ultralytics 导出的【原生 Detect】模型(DFL 在图内),
#   也就是算法组的 detect_exp/exp02_person_ebike_gpu2/weights/best_split.onnx。
#   (仓库里现成的 yolov8n_optimized.onnx 也是这个形态, 可以拿来试跑。)
#
# ★ 产物
#   onnx/<prefix>_fork9.onnx        9 输出 [box(64), cls(nc), sum(1)] x 3 尺度
#   models/<prefix>_fork9_int8.rknn  INT8 RKNN (toolkit 2.3.2, rk3588)
#
# ★ 依赖
#   · _bench/venv/bin/python3 (onnx / onnxruntime / numpy / rknn-toolkit2 2.3.2)
#   · 板端 runtime 也是 2.3.2, 版本必须一致, 否则模型加载不了
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO=/home/seeed/rk_deploy

SRC="${1:?用法: $0 <源 ONNX> [输出前缀]}"
PREFIX="${2:-$(basename "${SRC%.*}")}"

PY=${PY:-$REPO/_bench/venv/bin/python3}
CALIB=${CALIB:-$ROOT/calib/ebike_calib.txt}

echo "==> 源模型 : $SRC"
echo "==> 输出前缀: $PREFIX"
echo "==> 标定集 : $CALIB"

# ---- 1) 原生 Detect -> fork9(9 输出, DFL 移出图) ----
ONNX_OUT="$ROOT/onnx/${PREFIX}_fork9.onnx"
mkdir -p "$ROOT/onnx" "$ROOT/models" "$ROOT/out" "$ROOT/logs"
"$PY" -I "$HERE/export_fork9_from_detect.py" "$SRC" "$ONNX_OUT"

# ---- 2) 结构自检: 必须是 3 尺度 x [box(64), cls(nc), sum(1)] ----
"$PY" -I "$HERE/verify_fork9_structure.py" "$ONNX_OUT"

# ---- 3) 转 INT8 RKNN ----
RKNN_OUT="$ROOT/models/${PREFIX}_fork9_int8.rknn"
"$PY" -I "$HERE/convert_rknn_int8.py" "$ONNX_OUT" "$RKNN_OUT" --calib "$CALIB"

# ---- 4) 孪生的形态C(DFL 留图内) —— 做 A/B 的对照臂 ----
FORMC_ONNX="$ROOT/onnx/${PREFIX}_formC.onnx"
"$PY" -I "$HERE/make_dfl_in_npu_onnx_v3.py" "$ONNX_OUT" "$FORMC_ONNX"
"$PY" -I "$HERE/verify_formC_equiv.py" "$ONNX_OUT" "$FORMC_ONNX"
"$PY" -I "$HERE/convert_rknn_int8.py" "$FORMC_ONNX" \
       "$ROOT/models/${PREFIX}_formC_int8.rknn" --calib "$CALIB"

echo
echo "==> 完成:"
ls -la "$RKNN_OUT" "$ROOT/models/${PREFIX}_formC_int8.rknn"
echo "==> 下一步: bash $HERE/run_regression.sh ${PREFIX}_fork9_int8 ${PREFIX}_formC_int8"
