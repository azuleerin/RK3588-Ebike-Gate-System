#!/usr/bin/env bash
# run_ab_ocr.sh —— OCR 模型的生产流水线 A/B (4 路, max-fps=20 生产配置)
#   arm=prod  : 生产 OCR (ppocrv4_rec_fp16.rknn)
#   arm=mixed : 混合量化 OCR (候选模型)
set -uo pipefail
ROOT=/home/seeed/rk_deploy
OUT=$ROOT/opt_ocr_quant/out/ab
DURATION=${DURATION:-60}; WARMUP=${WARMUP:-15}; ROUNDS=${ROUNDS:-2}; MAXFPS=${MAXFPS:-20}
mkdir -p "$OUT"; cd "$ROOT/opt_dfl"
npu_idle() { sudo cat /sys/kernel/debug/rknpu/load 2>/dev/null \
    | grep -oE 'Core[0-9]+: *[0-9]+%' | grep -oE '[0-9]+' | tr '\n' ' '; }
wait_idle() {
  local i ok=0 loads v bad
  for i in $(seq 1 15); do
    loads="$(npu_idle)"; bad=0
    for v in $loads; do [ "$v" -gt 8 ] && bad=1; done
    if [ "$bad" -eq 0 ] && [ -n "$loads" ]; then
      ok=$((ok+1)); echo "    [$i] 每核: $loads (干净 $ok/3)"; [ "$ok" -ge 3 ] && return 0
    else ok=0; echo "    [$i] 每核: $loads (污染, 等)"; fi
    sleep 3
  done; return 1
}
for r in $(seq 1 "$ROUNDS"); do
  for arm in prod mixed; do
    if [ "$arm" = prod ]; then OCR=$ROOT/models/ppocrv4_rec_fp16.rknn; else OCR=$ROOT/opt_ocr_quant/models/ppocr_mixed_fp16_62.rknn; fi
    echo "=== 轮次 $r / $arm ==="
    sleep 10; wait_idle || { echo "    [!] 不干净, 作废"; continue; }
    python3 scripts/ab_harness.py --bin "$ROOT/rk3588_zero_copy/build/rk_gate" \
      --model "$ROOT/models/ebike_int8_rk2332.rknn" --labels "$ROOT/models/labels_ebike.txt" \
      --ocr "$OCR" --label "${arm}_r${r}" --duration "$DURATION" --warmup "$WARMUP" \
      --max-fps "$MAXFPS" --out "$OUT/${arm}_r${r}.json" 2>&1 \
      | grep -vE "RuntimeWarning|self.stderr|onnxruntime"
  done
done
echo "=== 完成, 结果在 $OUT ==="
