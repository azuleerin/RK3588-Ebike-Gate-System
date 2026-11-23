#!/usr/bin/env bash
# ============================================================================
# run_regression.sh —— 板端精度回归 + NPU 耗时 A/B
#
# 用法:
#     ./scripts/run_regression.sh [fork9模型前缀] [formC模型前缀]
#
# 默认:
#     fork9 模型 = models/ebike_nc2_fork9_int8.rknn
#     formC 模型 = models/ebike_nc2_formC_int8.rknn
#
# 产出(都在 out/ 和 logs/):
#   out/boxes_<tag>.csv      每张图的检测框
#   out/cmp_*.json           对比统计
#   logs/npu_ab_*.json       逐算子 NPU 耗时 A/B
#
# ★ 不碰生产: 只读 models/ 与 rk3588_zero_copy/src, 不改任何生产文件。
# ★ 不改板端配置: 不动 gate.env / GATE_MAX_FPS, 不重启服务。
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO=/home/seeed/rk_deploy
ACC=$REPO/accuracy

PY=${PY:-$REPO/_bench/venv/bin/python3}
FORK9_TAG=${1:-ebike_nc2_fork9}
FORMC_TAG=${2:-ebike_nc2_formC}
PROD=$REPO/models/ebike_int8_rk2332.rknn

ACC_BIN=$ROOT/src/acc_batch_nc          # nc 自适应的回归解码器(沙盒副本)
# ★ frames_list.txt 里是【相对路径】(data/frames/f00000.jpg) —— 必须 cd 到 accuracy/
#   再跑, 这样 CSV 里的帧路径和仓库里已有的 out/boxes_*.csv 口径一致
#   (aggregate.py 是按【帧路径字符串】join 的, 路径口径不一致会 join 不上 -> 匹配率 0)。
FRAMES=data/manifests/frames_list.txt
export LD_LIBRARY_PATH=$REPO/rk3588_zero_copy/third_party/lib

mkdir -p "$ROOT/out" "$ROOT/logs"

run_yolo() {   # $1=模型路径 $2=tag
    local m="$1" tag="$2"
    [ -f "$m" ] || { echo "  跳过(模型不存在): $m"; return 0; }
    if [ -s "$ROOT/out/boxes_$tag.csv" ]; then
        echo "  跳过(已存在): out/boxes_$tag.csv"; return 0
    fi
    echo "  -> $tag"
    # cd 到 accuracy/ —— 帧清单是相对路径, 且要保证 CSV 里的路径口径与仓库一致
    ( cd "$ACC" && "$ACC_BIN" yolo --model "$m" --list "$FRAMES" \
        --out "$ROOT/out/boxes_$tag.csv" --conf 0.25 --nms 0.45 --size 640x640 \
        2>&1 | tail -2 )
}

echo "=============== 1) 板端批量推理 (1724 帧) ==============="
run_yolo "$ROOT/models/${FORK9_TAG}_int8.rknn" "$FORK9_TAG"
run_yolo "$ROOT/models/${FORMC_TAG}_int8.rknn" "$FORMC_TAG"
run_yolo "$PROD" prod_ebike

echo
echo "=============== 2) 精度回归 ==============="
# 2.1 同权重 A/B: 形态C(DFL在NPU) vs fork9(DFL在CPU) —— 唯一变量是 DFL 在哪
if [ -s "$ROOT/out/boxes_$FORMC_TAG.csv" ] && [ -s "$ROOT/out/boxes_$FORK9_TAG.csv" ]; then
    echo "--- A) 同权重 A/B (唯一变量 = DFL 解码的位置) ---"
    "$PY" -I "$HERE/compare_boxes.py" \
        --ref "$ROOT/out/boxes_$FORMC_TAG.csv" --test "$ROOT/out/boxes_$FORK9_TAG.csv" \
        --ref-label "$FORMC_TAG (DFL in NPU)" --label "$FORK9_TAG (DFL on CPU)" \
        --json "$ROOT/out/cmp_formC_vs_fork9.json"
fi
# 2.2 与生产模型对比(权重不同, 只作参照)
if [ -s "$ROOT/out/boxes_prod_ebike.csv" ] && [ -s "$ROOT/out/boxes_$FORK9_TAG.csv" ]; then
    echo
    echo "--- B) 与生产 ebike 模型对比 (权重不同, 仅作参照) ---"
    "$PY" -I "$HERE/compare_boxes.py" \
        --ref "$ROOT/out/boxes_prod_ebike.csv" --test "$ROOT/out/boxes_$FORK9_TAG.csv" \
        --ref-label "生产 ebike_int8_rk2332" --label "$FORK9_TAG" \
        --json "$ROOT/out/cmp_prod_vs_fork9.json"
fi

echo
echo "=============== 3) NPU 耗时 A/B (RKNN_QUERY_PERF_DETAIL) ==============="
for m in "$PROD" "$ROOT/models/${FORK9_TAG}_int8.rknn" "$ROOT/models/${FORMC_TAG}_int8.rknn"; do
    [ -f "$m" ] || continue
    tag=$(basename "$m" .rknn)
    echo "  -> $tag"
    "$PY" -I "$REPO/audit/model_audit.py" "$m" --no-toolkit \
        --json "$ROOT/logs/npu_ab_$tag.json" --quiet 2>&1 | tail -1
done
"$PY" -I "$HERE/summarize_npu_ab.py" "$ROOT/logs" \
    "$PROD" "$ROOT/models/${FORK9_TAG}_int8.rknn" "$ROOT/models/${FORMC_TAG}_int8.rknn" \
    | tee "$ROOT/logs/npu_ab_summary.txt"

echo
echo "=============== 完成 ==============="
ls -la "$ROOT/out"/*.csv "$ROOT/out"/*.json 2>/dev/null
