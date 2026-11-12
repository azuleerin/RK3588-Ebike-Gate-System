#!/usr/bin/env bash
# bench_all.sh —— 重启后一键完成: YOLOv8n 纯 NPU 基准 + 频率锁定 A/B 对比
#
# 用法:
#   sudo reboot          # 先重启恢复 NPU (power/control=on 触发的 IOMMU abort 会死锁 NPU)
#   ./bench_all.sh       # 然后跑这个
#
# 输出两组数字:
#   [A] 默认 ondemand 调度
#   [B] 频率锁定 (NPU 1G / DDR 2.4G / CPU performance)
# 差值 = 频率锁定的真实收益

set -u
cd "$(dirname "$0")"

MODEL="${1:-./yolov8n_rk3588_int8.rknn}"
ITERS="${2:-200}"
WARM="${3:-30}"
LOCK="./freq_lock.sh"

if [ ! -f "$MODEL" ]; then
  echo "找不到模型: $MODEL"
  echo "用法: $0 [model.rknn] [iters] [warmup]"
  exit 2
fi

echo "############################################################"
echo "# A. 默认调度 (ondemand)"
echo "############################################################"
./freq_lock.sh show
echo
./npu_bench "$MODEL" --iters "$ITERS" --warmup "$WARM" --core 1
A=$?

echo
echo "############################################################"
echo "# B. 频率锁定 (NPU 1GHz / DDR 2.4GHz / CPU performance)"
echo "############################################################"
if echo '123456' | sudo -S -p "" "$LOCK" lock 2>/dev/null; then
  ./freq_lock.sh show
  echo
  ./npu_bench "$MODEL" --iters "$ITERS" --warmup "$WARM" --core 1
  B=$?
else
  echo "[跳过] 需要 root 才能锁频率, 请用: sudo ./bench_all.sh"
  B=1
fi

echo
echo "############################################################"
echo "# C. 逐层耗时 (定位慢算子)"
echo "############################################################"
./npu_bench "$MODEL" --iters 50 --warmup 10 --core 1 --perfdetail

if [ "${B:-0}" = "0" ]; then
  echo
  echo ">>> 想还原默认调度: sudo ./freq_lock.sh restore"
fi
exit $(( ${A:-0} | ${B:-0} ))
