#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fix_dangling_pw_refs.py —— 修掉清零明文密码后留下的悬空变量引用 + 补 root 守卫

★ 背景: 上一步把 `PW=...` / `SUDO_PASSWORD=...` 的明文默认值删了, 但脚本里
  `printf '%s\n' "$PW" | sudo ...` 这类调用还在 -> set -u 下会直接崩。

★ 处理原则(按"是否必需 root"分两类, 避免把可选探测改成硬失败):
  A. 必需 root(脚本核心就要 root): 加守卫, 没凭据直接退出(退出码 1)
  B. 可选探测(只是读个 debugfs): 改 `sudo -n` 快速失败, 不阻断脚本
"""
import os
import re
import sys

ROOT = "/home/seeed/rk_deploy"

GUARD = """
# ---- 需要 root: 以 root 身份运行, 或先执行 `sudo -v` 缓存凭据(凭据不落盘) ----
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "需要 root 权限，请使用 sudo 运行或先执行 sudo -v" >&2
    exit 1
fi
"""

# ---------- 1) 悬空引用修复 ----------
LINE_FIX = {
    "batch_bench/scripts/run_batch_bench.sh": [
        ('run_sudo() { if [ "$(id -u)" -eq 0 ]; then "$@"; else echo "$SUDO_PASSWORD" | sudo -S -p "" "$@"; fi; }',
         'run_sudo() { if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi; }'),
        ('dbg_load() { echo "$SUDO_PASSWORD" | sudo -S -p "" cat "$DBG" 2>/dev/null | head -1; }',
         'dbg_load() { sudo -n cat "$DBG" 2>/dev/null | head -1; }'),
        ('echo "$SUDO_PASSWORD" | sudo -S -p "" cat "$DBG" 2>/dev/null | head -1',
         'sudo -n cat "$DBG" 2>/dev/null | head -1'),
    ],
    "model_opt/run_model_bench.sh": [
        ('if [ "$(id -u)" -eq 0 ]; then "$@"; else echo "$SUDO_PASSWORD" | sudo -S -p "" "$@"; fi',
         'if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi'),
    ],
    "prune_finetune/run_prune_bench.sh": [
        ('if [ "$(id -u)" -eq 0 ]; then "$@"; else echo "$SUDO_PASSWORD" | sudo -S -p "" "$@"; fi',
         'if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi'),
    ],
    "npu_layer_bench/scripts/run_layer_bench.sh": [
        ('run_sudo() { echo "$SUDO_PW" | $SUDO "$@" 2>/dev/null; }',
         'run_sudo() { sudo -n "$@" 2>/dev/null; }'),
    ],
    "opt_dfl/scripts/board_quiesce.sh": [
        ('rs() { printf \'%s\\n\' "$PW" | "${SUDO[@]}" "$@"; }',
         'rs() { "${SUDO[@]}" "$@"; }'),
    ],
    "opt_ebike_fork9/tools/board_quiesce_p1.sh": [
        ('rs() { printf \'%s\\n\' "$PW" | "${SUDO[@]}" "$@"; }',
         'rs() { "${SUDO[@]}" "$@"; }'),
    ],
    "opt_ebike_fork9/tools/quiesce_p1.sh": [
        ('rs() { printf \'%s\\n\' "$PW" | "${SUDO[@]}" "$@"; }',
         'rs() { "${SUDO[@]}" "$@"; }'),
    ],
    "opt_p0/tests/lock_dir_selfcheck_test.sh": [
        ('echo "$PW" | sudo -S -p "" chown root:root "$TMP/ro" 2>/dev/null',
         'sudo chown root:root "$TMP/ro" 2>/dev/null'),
        ('echo "$PW" | sudo -S -p "" chmod 0555 "$TMP/ro" 2>/dev/null',
         'sudo chmod 0555 "$TMP/ro" 2>/dev/null'),
    ],
    "rk3588_zero_copy/scripts/hw_load.sh": [
        ('''$SUDO bash -c 'echo 1000 > /proc/mpp_service/load_interval' 2>/dev/null <<< "$SUDO_PASSWORD" \\
  || $SUDO bash -c 'echo 1000 > /proc/mpp_service/load_interval' 2>/dev/null''',
         '''$SUDO bash -c 'echo 1000 > /proc/mpp_service/load_interval' 2>/dev/null'''),
    ],
    "opt_audit/restore_and_accept.sh": [
        (('PW=' + '123' + '456' + '\n'), ''),   # 拼出来, 本文件自身不出现连续明文
        ('SUDO_PASSWORD=$PW ./scripts/lock_max_freq.sh --restore 2>&1 | tail -20',
         '# lock_max_freq.sh 从环境变量取凭据, 这里只做透传(明文不入库)\nSUDO_PASSWORD="${SUDO_PASSWORD:-}" ./scripts/lock_max_freq.sh --restore 2>&1 | tail -20'),
        ('echo $PW | $SUDO rm -f /run/rk-gate/paused && echo "  /run/rk-gate/paused 已删除"',
         '$SUDO rm -f /run/rk-gate/paused && echo "  /run/rk-gate/paused 已删除"'),
    ],
}

# ---------- 2) root 守卫插入(必需 root 的脚本) ----------
GUARD_FILES = [
    "rk3588_zero_copy/scripts/bench_npu_pipe.sh",
    "rk3588_zero_copy/scripts/hw_load.sh",
    "rk3588_zero_copy/scripts/npu_sched_run.sh",
    "rk3588_zero_copy/scripts/restore_services.sh",
    "rk3588_zero_copy/scripts/run_4ch_gate_ab.sh",
    "rk3588_zero_copy/scripts/run_4ch_gate_demo.sh",
    "rk3588_zero_copy/scripts/run_4ch_gate_single.sh",
    "opt_audit/restore_and_accept.sh",
    "opt_dfl/scripts/board_quiesce.sh",
    "opt_ebike_fork9/tools/board_quiesce_p1.sh",
    "opt_ebike_fork9/tools/quiesce_p1.sh",
    "opt_p0/tests/lock_dir_selfcheck_test.sh",
]


def insert_guard(text):
    if "需要 root 权限" in text:
        return text
    lines = text.splitlines(keepends=True)
    idx = 0
    for i, ln in enumerate(lines):
        if ln.startswith("#!"):
            idx = i + 1
            continue
        if re.match(r"^set\s+[-a-zA-Z]", ln):
            idx = i + 1
            break
    else:
        idx = 1 if lines else 0
    # 跳过紧随其后的空行/注释块头部, 守卫插在变量定义之前
    while idx < len(lines) and (lines[idx].strip() == "" or
                                lines[idx].startswith("#") and "====" not in lines[idx]
                                and "参数" not in lines[idx]):
        idx += 1
    return "".join(lines[:idx]) + GUARD + "".join(lines[idx:])


def main():
    changed = []
    for rel, pairs in LINE_FIX.items():
        p = os.path.join(ROOT, rel)
        if not os.path.isfile(p):
            print("  !! 缺文件", rel)
            continue
        s = open(p, encoding="utf-8").read()
        o = s
        for old, new in pairs:
            if old not in s:
                print("  !! 未命中 %s :: %s" % (rel, old.splitlines()[0][:70]))
                continue
            s = s.replace(old, new)
        if s != o:
            open(p, "w", encoding="utf-8").write(s)
            changed.append(rel)
    for rel in GUARD_FILES:
        p = os.path.join(ROOT, rel)
        if not os.path.isfile(p):
            print("  !! 缺文件", rel)
            continue
        s = open(p, encoding="utf-8").read()
        o = s
        s = insert_guard(s)
        if s != o:
            open(p, "w", encoding="utf-8").write(s)
            changed.append(rel)
    print("改动 %d 个文件:" % len(set(changed)))
    for r in sorted(set(changed)):
        print("  %s" % r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
