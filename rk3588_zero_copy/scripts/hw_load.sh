#!/usr/bin/env bash
# hw_load.sh —— NPU / VPU(MPP) / CPU / 流水线 FPS 实时采样 + ASCII 仪表盘
#
# 用途: 回答"瓶颈到底在哪" —— NPU 三核各占多少、VPU 编解码占多少、
#       流水线跑到多少 FPS。
#
# 用法:
#   sudo scripts/hw_load.sh [秒数] [CSV输出]
#
# 数据源(全部是内核/驱动直接给的, 不是猜的):
#   NPU : /sys/kernel/debug/rknpu/load        (需要 root)
#   VPU : /proc/mpp_service/load               (需要 root, 且要先设 load_interval)
#   CPU : /proc/stat
#   FPS : 主程序日志里的 [frame N] 行
set -u

DURATION=${1:-30}
CSV=${2:-/home/seeed/rk_deploy/logs/hw_load.csv}
LOG=${LOG:-/home/seeed/rk_deploy/logs/memtest.log}
SUDO="sudo -S"

# ---- 需要 root 的部分 ----
$SUDO bash -c 'echo 1000 > /proc/mpp_service/load_interval' 2>/dev/null <<< "$SUDO_PASSWORD" \
  || echo 123456 | $SUDO bash -c 'echo 1000 > /proc/mpp_service/load_interval' 2>/dev/null

echo "time,npu0,npu1,npu2,vpu_dec,cpu_pct,fps,frame" > "$CSV"

# 采样
prev_cpu_total=0; prev_cpu_idle=0
for _ in $(seq 1 "$DURATION"); do
    now=$(date +%s)
    # NPU 三核
    npu=$(echo 123456 | $SUDO cat /sys/kernel/debug/rknpu/load 2>/dev/null)
    n0=$(echo "$npu" | sed -nE 's/.*Core0: *([0-9]+)%.*/\1/p'); n0=${n0:-0}
    n1=$(echo "$npu" | sed -nE 's/.*Core1: *([0-9]+)%.*/\1/p'); n1=${n1:-0}
    n2=$(echo "$npu" | sed -nE 's/.*Core2: *([0-9]+)%.*/\1/p'); n2=${n2:-0}
    # VPU: 取 rkvdec(解码核) 的 load
    vpu=$(echo 123456 | $SUDO cat /proc/mpp_service/load 2>/dev/null | \
          awk '/rkvdec/{gsub("%","",$3); if($3+0>max) max=$3+0} END{print max+0}')
    # CPU 占用率(两次采样之间的差值)
    # /proc/stat 的 cpu 行: user nice system idle iowait irq softirq steal guest guest_nice
    # ⚠️ 字段别数错: 少读一个 user, idle 就会取到 iowait(≈0) -> 算成 100%
    cpu_line=$(awk '/^cpu /{print $2,$3,$4,$5,$6,$7,$8,$9,$10,$11}' /proc/stat)
    read -r u1 n1c s1 i1 w1 ir1 sr1 st1 g1 gn1 <<< "$cpu_line"
    total=$((u1+n1c+s1+i1+w1+ir1+sr1+st1+g1+gn1)); idle=$((i1+w1))
    dt=$((total-prev_cpu_total)); di=$((idle-prev_cpu_idle))
    if [ "$dt" -gt 0 ]; then cpu=$(awk -v d="$dt" -v i="$di" 'BEGIN{printf "%.1f", (1-i/d)*100}'); else cpu=0; fi
    prev_cpu_total=$total; prev_cpu_idle=$idle
    # 流水线 FPS / 帧号
    last=$(grep -E "^\[frame [0-9]+\]" "$LOG" 2>/dev/null | tail -1)
    fps=$(echo "$last" | sed -nE 's/.*fps=([0-9.]+).*/\1/p'); fps=${fps:-0}
    fr=$(echo "$last" | sed -nE 's/^\[frame ([0-9]+)\].*/\1/p'); fr=${fr:-0}
    echo "${now},${n0},${n1},${n2},${vpu},${cpu},${fps},${fr}" >> "$CSV"
    sleep 1
done

# ---- ASCII 仪表盘 ----
echo ""
echo "══════════════ RK3588 硬件负载仪表盘 ($DURATION 秒采样) ══════════════"
printf "%-8s %6s %6s %6s %8s %7s %8s\n" "时间" "NPU0" "NPU1" "NPU2" "VPU解码" "CPU%" "FPS"
awk -F, 'NR>1{n++; printf "%-8s %6s %6s %6s %8s %7s %8s\n", strftime("%H:%M:%S",$1), $2, $3, $4, $5, $6, $7}' "$CSV" | tail -20

echo ""
awk -F, 'NR>1{n0+=$2;n1+=$3;n2+=$4;v+=$5;c+=$6;f+=$7; if($7>maxf)maxf=$7; if(n==0||$8<minfr)minfr=$8; if($8>maxfr)maxfr=$8; n++}
END{
  printf "  NPU Core0 平均 %5.1f%%   Core1 %5.1f%%   Core2 %5.1f%%\n", n0/n, n1/n, n2/n;
  printf "  VPU(rkvdec) 平均 %5.1f%%\n", v/n;
  printf "  CPU 平均 %5.1f%%\n", c/n;
  printf "  流水线 平均 FPS %5.2f   峰值 FPS %5.2f   帧号 %d -> %d\n", f/n, maxf, minfr, maxfr;
}' "$CSV"

echo ""
echo "  判读:"
awk -F, 'NR>1{n0+=$2;n1+=$3;n2+=$4;v+=$5;c+=$6;f+=$7;n++}
END{
  n0/=n; n1/=n; n2/=n; v/=n; c/=n; f/=n;
  if (n0 > 80) print "    ★ NPU Core0 接近满载 -> 算力瓶颈(换小模型/批处理/量化)";
  else if (n0 > 40) print "    ● NPU Core0 中等占用("n0"%) -> NPU 不是唯一瓶颈, 但占大头";
  else print "    ○ NPU Core0 占用不高("n0"%) -> NPU 空闲, 瓶颈在别处";
  if (v < 20) print "    ○ VPU 解码核几乎空闲("v"%) -> 硬解完全不是瓶颈";
  else print "    ● VPU 解码核占用 "v"%";
  if (c > 70) print "    ● CPU 平均 "c"% -> 后处理/搬运在吃 CPU";
}' "$CSV"
echo "════════════════════════════════════════════════════════════════"
