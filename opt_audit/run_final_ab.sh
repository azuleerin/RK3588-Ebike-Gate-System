#!/usr/bin/env bash
# run_final_ab.sh —— 最终 A/B: 吞吐口径(不限帧) + 生产口径(max-fps=20)
set -uo pipefail
cd /home/seeed/rk_deploy/opt_audit
rm -rf ab_final && mkdir -p ab_final

echo "############ ① 吞吐口径 (max-fps=0, 180s/轮, 兼作 3 分钟内存平线测试) ############"
DURATION=180 WARMUP=20 ROUNDS=3 MAXFPS=0 ./run_ab.sh

echo ""
echo "############ ② 生产口径 (max-fps=20, 100s/轮) ############"
DURATION=100 WARMUP=15 ROUNDS=3 MAXFPS=20 ./run_ab.sh
echo ""
echo "=== 全部完成 ==="
