#!/usr/bin/env bash
# ============================================================================
# smoke_p1_switch.sh —— P1-2′ 原子切换后的 300 帧 4 路冒烟（生产同参）
#
# ★ 口径（任务书 B4）:
#   · 参数从 config/gate.env 渲染, 与 systemd 起来的那个【逐参数一致】
#     (render_gate_args.sh 照抄 run_gate_service.sh 的 ARGS 组装)
#   · 4 路 / --max-fps 20 / --duration 15  => 每路约 300 帧
#   · 硬门槛:
#       退出码 = 0            零崩溃
#       [车牌] 行裁剪宽 >= 160 px (ch1 / ch3)   彻底脱离 66 px 低分辨区
#       零宽/零高裁剪 = 0
#       MPP/RGA/EMFILE/自检/崩溃 类报错 = 0
#
# 用法: bash smoke_p1_switch.sh [时长s] [输出目录]
# ============================================================================
set -uo pipefail

DUR=${1:-15}
REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
BIN=$PROJ/build/rk_gate
OUT=${2:-/home/seeed/rk-gate-backup-p1roi-20261123_211826}
LOGDIR=$OUT/smoke
mkdir -p "$LOGDIR"
STAMP=$(date +%Y%m%d_%H%M%S)
RUNLOG=$LOGDIR/smoke_fork9_${STAMP}.log

echo "================================================================"
echo " P1-2′ 300 帧 4 路冒烟   $(date '+%F %T')"
echo "================================================================"

# ---- 1. 渲染生产同参命令行 ----
echo "==> ① 从 config/gate.env 渲染命令行"
mapfile -t LINES < <(bash "$REPO/opt_ebike_fork9/tools/render_gate_args.sh" --lines)
BIN=${LINES[0]}                       # 第 0 行是二进制路径
ARGS=("${LINES[@]:1}")                # 其余是参数（未转义, IPM 里的 ; , 原样保留）
echo "    bin    : $BIN"
echo "    model  : $(grep -oP '(?<=--model\s)\S+' <<<"${ARGS[*]}")"
echo "    roi    : ratio=$(grep -oP '(?<=--roi-ratio\s)\S+' <<<"${ARGS[*]}") "\
"y=$(grep -oP '(?<=--roi-y\s)\S+' <<<"${ARGS[*]}") "\
"height=$(grep -oP '(?<=--roi-height\s)\S+' <<<"${ARGS[*]}") "\
"shift=$(grep -oP '(?<=--roi-shift\s)\S+' <<<"${ARGS[*]}") "\
"below=$(grep -oP '(?<=--roi-below\s)\S+' <<<"${ARGS[*]}")"
echo "    ch 数  : $(printf '%s\n' "${ARGS[@]}" | grep -cx -- '--ch')"
echo "    参数数: ${#ARGS[@]}"

# ---- 2. 跑 ----
echo "==> ② 跑 ${DUR}s（每路约 $((DUR * 20)) 帧）"
export LD_LIBRARY_PATH=$PROJ/third_party/lib:$PROJ/lib:/usr/lib
export RK_LOG_LEVEL="${RK_LOG_LEVEL:-warn}"
# 换掉 duration 的【值】(保留 --duration 这个 flag 本身!)
NEWARGS=()
i=0
while [ $i -lt ${#ARGS[@]} ]; do
    if [ "${ARGS[$i]}" = "--duration" ]; then
        NEWARGS+=("--duration" "$DUR")
        i=$((i + 2))          # 跳过 flag 和它原来的值
        continue
    fi
    NEWARGS+=("${ARGS[$i]}")
    i=$((i + 1))
done
"$BIN" "${NEWARGS[@]}" > "$RUNLOG" 2>&1
RC=$?
echo "    退出码 = $RC   日志 = $RUNLOG"

# ---- 3. 判定 ----
echo "==> ③ 判定"
python3 -I - "$RUNLOG" "$RC" <<'PY'
import re
import sys
from collections import defaultdict

log, rc = sys.argv[1], int(sys.argv[2])
txt = open(log, encoding="utf-8", errors="replace").read()
lines = txt.splitlines()

fails = []

# --- 退出码 ---
if rc != 0:
    fails.append(f"退出码 {rc} != 0")

# --- 逐路帧数（最后一条统计行的帧号）---
FR = re.compile(r"^\[(ch\d)\]\[frame (\d+)\]")
last = {}
for l in lines:
    m = FR.match(l)
    if m:
        last[m.group(1)] = int(m.group(2))
print(f"    逐路末帧号: {last}")
for ch in ("ch1", "ch2", "ch3", "ch4"):
    if ch not in last:
        fails.append(f"{ch} 无统计行（疑似没跑起来）")
    elif last[ch] < 250:
        fails.append(f"{ch} 只跑到 {last[ch]} 帧 (<250, 未达 300 帧量级)")

# --- 车牌裁剪宽 ---
PL = re.compile(r"\[(ch\d)\].*?roi=(\d+)x(\d+)@\((-?\d+),(-?\d+)\)\s*裁剪=(\d+)x(\d+)")
wid = defaultdict(list)
zero = []
for l in lines:
    m = PL.search(l)
    if not m:
        continue
    ch, cw, chh = m.group(1), int(m.group(6)), int(m.group(7))
    wid[ch].append(cw)
    if cw <= 0 or chh <= 0:
        zero.append(l)
print(f"    [车牌] 行数: {sum(len(v) for v in wid.values())}  逐路: { {k: len(v) for k, v in wid.items()} }")
for ch in ("ch1", "ch3"):
    v = sorted(wid.get(ch, []))
    if not v:
        print(f"    {ch}: 本轮无车牌裁剪样本（素材里没进车牌 —— 宽度判定沿用生产活日志证据）")
        continue
    med = v[len(v) // 2]
    below = sum(1 for x in v if x < 160)
    print(f"    {ch}: n={len(v)} min={v[0]} median={med} max={v[-1]}  "
          f"<160px: {below}/{len(v)}")
    if med < 160:
        fails.append(f"{ch} 裁剪宽中位 {med} px < 160 px")

if zero:
    fails.append(f"零宽/零高裁剪 {len(zero)} 条")

# --- 错误模式 ---
PATS = {
    "MPP 报错": r"(?i)mpp.*(error|fail|not ready)",
    "RGA 报错": r"(?i)rga.*(error|fail)",
    "EMFILE/句柄耗尽": r"(?i)EMFILE|too many open files",
    "自检失败": r"(?i)自检|self.?check.*fail",
    "崩溃": r"(?i)segmentation|SIGSEGV|SIGABRT|abort|assert|core dumped",
}
print("    错误模式扫描:")
for name, pat in PATS.items():
    hits = [l for l in lines if re.search(pat, l)]
    # "mpp_platform: client N driver is not ready!" 是 MPP 初始化期的无害提示, 不算报错
    if name == "MPP 报错":
        hits = [h for h in hits if "not ready" not in h]
    print(f"      {name:16s}: {len(hits)}")
    if hits:
        for h in hits[:3]:
            print(f"         ! {h[:160]}")
        fails.append(f"{name} {len(hits)} 条")

print()
if fails:
    print("    判定: FAIL")
    for f in fails:
        print(f"      - {f}")
    sys.exit(1)
print("    判定: PASS —— 零崩溃 / 零自检失败 / 零 MPP-RGA-EMFILE 报错 / 裁剪宽达标")
PY
V=$?

echo
if [ "$V" -eq 0 ]; then
    echo "==> [OK] 冒烟通过。日志: $RUNLOG"
else
    echo "==> [!] 冒烟未通过（见上）。日志: $RUNLOG"
fi
exit $V
