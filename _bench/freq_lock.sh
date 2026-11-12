#!/usr/bin/env bash
# freq_lock.sh —— RK3588 NPU 性能锁定 / 还原 (Rockchip 官方 benchmark 同款条件)
#
# 用法:
#   sudo ./freq_lock.sh lock    # 锁定到官方基准条件 (NPU 1GHz / DDR 2.4GHz / CPU performance)
#   sudo ./freq_lock.sh restore # 还原成 ondemand 调度 (省电默认)
#
# 官方依据: rknn_model_zoo/scaling_frequency.sh
#   "This performance data are collected based on the maximum NPU frequency of each platform."
#   RK3588 目标: NPU 1000000000 / CPU 2256000000 / DDR 2112000000
#   (本板 DDR 可选档位为 534M/1320M/1968M/2400M, 取最高 2400M; CPU 大核最高 2304M, 取 performance)

set -u
MODE="${1:-lock}"

NPU_DEV=/sys/class/devfreq/fdab0000.npu
DDR_DEV=/sys/class/devfreq/dmc
CPU_DIR=/sys/devices/system/cpu

npu_freq_max() { cat "$NPU_DEV/max_freq" 2>/dev/null || echo 1000000000; }
ddr_freq_max() { cat "$DDR_DEV/max_freq" 2>/dev/null || echo 2400000000; }

case "$MODE" in
  lock)
    echo "== NPU: governor=userspace, freq=$(npu_freq_max) =="
    echo userspace > "$NPU_DEV/governor" 2>/dev/null
    echo "$(npu_freq_max)" > "$NPU_DEV/userspace/set_freq" 2>/dev/null

    echo "== DDR: governor=userspace, freq=$(ddr_freq_max) =="
    echo userspace > "$DDR_DEV/governor" 2>/dev/null
    echo "$(ddr_freq_max)" > "$DDR_DEV/userspace/set_freq" 2>/dev/null

    echo "== CPU: governor=performance (含大核 A76) =="
    for f in "$CPU_DIR"/cpu*/cpufreq/scaling_governor; do
      echo performance > "$f" 2>/dev/null
    done
    # CPU idle 也要压住, 否则空转时进低功耗态
    for f in "$CPU_DIR"/cpuidle/current_governor; do
      echo performance > "$f" 2>/dev/null
    done

    # NPU runtime PM: suspended 状态下每次推理首帧都有唤醒延迟
    NPU_PM=/sys/devices/platform/fdab0000.npu/power
    if [ -w "$NPU_PM/control" ]; then
      echo on > "$NPU_PM/control" 2>/dev/null && echo "== NPU runtime PM: on =="
    fi
    ;;

  restore)
    echo "== 还原为 ondemand =="
    echo rknpu_ondemand > "$NPU_DEV/governor" 2>/dev/null
    echo dmc_ondemand > "$DDR_DEV/governor" 2>/dev/null
    for f in "$CPU_DIR"/cpu*/cpufreq/scaling_governor; do
      echo ondemand > "$f" 2>/dev/null
    done
    for f in "$CPU_DIR"/cpuidle/current_governor; do
      echo menu > "$f" 2>/dev/null
    done
    NPU_PM=/sys/devices/platform/fdab0000.npu/power
    if [ -w "$NPU_PM/control" ]; then
      echo auto > "$NPU_PM/control" 2>/dev/null
    fi
    ;;

  show)
    echo "NPU gov=$(cat $NPU_DEV/governor) cur=$(cat $NPU_DEV/cur_freq) max=$(cat $NPU_DEV/max_freq)"
    echo "DDR gov=$(cat $DDR_DEV/governor) cur=$(cat $DDR_DEV/cur_freq) max=$(cat $DDR_DEV/max_freq)"
    echo "CPU gov(cpu4)=$(cat $CPU_DIR/cpu4/cpufreq/scaling_governor) cur=$(cat $CPU_DIR/cpu4/cpufreq/scaling_cur_freq) max=$(cat $CPU_DIR/cpu4/cpufreq/scaling_max_freq)"
    echo "CPU idle=$(cat $CPU_DIR/cpuidle/current_governor 2>/dev/null)"
    echo "NPU PM=$(cat /sys/devices/platform/fdab0000.npu/power/runtime_status 2>/dev/null) control=$(cat /sys/devices/platform/fdab0000.npu/power/control 2>/dev/null)"
    ;;

  *) echo "用法: $0 {lock|restore|show}"; exit 2;;
esac
