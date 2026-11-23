#!/usr/bin/env bash
# ============================================================================
# make_res_models.sh —— 生成分辨率扫描用的 RKNN 模型(640 / 576 / 512)
#
# 为什么要重新编译:
#   生产模型(ebike_int8_rk2332.rknn)是固定 640x640 编译的, 输入尺寸烤死
#   在模型里 —— 想测 576/512 就只能回到 ONNX 重转。
#
# 用哪个 ONNX:
#   yolov8n_optimized.onnx(和 _bench/check0_base_optimize.onnx 同族)。
#   ★ 它是【1 类】的 YOLOv8n, 生产模型是【2 类】—— 差别只在检测头
#     (model.22.cv3.*)的那几个卷积, 而这部分计算量在三种分辨率下
#     是个【常数】, 所以"分辨率 -> 帧率"这条【缩放曲线】不受影响。
#     报告里会同时给出本 ONNX 在 640 的实测值作为锚点, 便于换算。
#
# ★ 为什么"换分辨率"这件事能代表生产模型:
#   INT8 卷积的耗时由【形状】决定, 与权重取值无关(数据无关的 GEMM)。
#   所以同一个网络结构换输入尺寸, 每帧计算量按面积缩放,
#   测出来的分辨率-帧率曲线对生产模型同样成立。
#
# 依赖:
#   · _bench/venv/bin/python3 (有 onnx / onnxruntime / numpy)
#   · rknn-toolkit2 2.3.2 的 wheel(板端 aarch64 版), 解压后用 PYTHONPATH 指过去
#     ★ 版本必须和板上的 runtime(2.3.2)一致, 否则模型加载失败。
#
# 用法:
#   scripts/make_res_models.sh [640 576 512]
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

VENV_PY=${VENV_PY:-/home/seeed/rk_deploy/_bench/venv/bin/python3}
ONNX_SRC=${ONNX_SRC:-/home/seeed/rk_deploy/_bench/check0_base_optimize.onnx}
CALIB=${CALIB:-/home/seeed/rk_deploy/_bench/calib_list.txt}
WHEEL=${WHEEL:-/home/seeed/rk_deploy/_bench/wheel/rknn_toolkit2-2.3.2-cp312-cp312-manylinux_2_17_aarch64.manylinux2014_aarch64.whl}
TOOLKIT_DIR=${TOOLKIT_DIR:-$ROOT/.toolkit}
OUT_DIR=${OUT_DIR:-$ROOT/models/res}

SIZES=("$@")
[ ${#SIZES[@]} -eq 0 ] && SIZES=(640 576 512)

# ---- 准备 rknn-toolkit2 ----
if [ ! -d "$TOOLKIT_DIR/rknn" ]; then
    echo "==> 解压 rknn-toolkit2 到 $TOOLKIT_DIR"
    mkdir -p "$TOOLKIT_DIR"
    unzip -oq "$WHEEL" -d "$TOOLKIT_DIR"
fi
export PYTHONPATH="$TOOLKIT_DIR${PYTHONPATH:+:$PYTHONPATH}"

mkdir -p "$OUT_DIR"

for SZ in "${SIZES[@]}"; do
    ONNX_OUT="$OUT_DIR/yolov8n_${SZ}.onnx"
    RKNN_OUT="$OUT_DIR/yolov8n_${SZ}.rknn"

    if [ -f "$RKNN_OUT" ]; then
        echo "==> $SZ: 已存在, 跳过"
        continue
    fi

    echo ""
    echo "==================== 尺寸 $SZ ===================="
    "$VENV_PY" "$HERE/resize_onnx.py" "$ONNX_SRC" "$ONNX_OUT" --size "$SZ" --verify

    echo "==> 转 RKNN (toolkit 2.3.2, INT8)"
    "$VENV_PY" - "$ONNX_OUT" "$RKNN_OUT" "$CALIB" <<'PYEOF'
import sys
from rknn.api import RKNN

src, dst, calib = sys.argv[1], sys.argv[2], sys.argv[3]

rknn = RKNN(verbose=False)
# Rockchip 官方口径(rknn_model_zoo/examples/yolov8/python/convert.py)
ret = rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]],
                  target_platform="rk3588")
if ret != 0:
    sys.exit(f"config failed: {ret}")
ret = rknn.load_onnx(model=src)
if ret != 0:
    sys.exit(f"load_onnx failed: {ret}")
ret = rknn.build(do_quantization=True, dataset=calib)
if ret != 0:
    sys.exit(f"build failed: {ret}")
ret = rknn.export_rknn(dst)
if ret != 0:
    sys.exit(f"export_rknn failed: {ret}")
rknn.release()
print(f">>> 已写出 {dst}")
PYEOF
done

echo ""
echo "==> 完成。产物:"
ls -la "$OUT_DIR"
