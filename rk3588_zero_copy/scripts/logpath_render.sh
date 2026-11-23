#!/usr/bin/env bash
# ============================================================================
# logpath_render.sh —— 把仓库模板里的 /var/log/ 前缀渲染成实际日志目录
#
# ★★ 为什么要有这个共享件(2026-11-23 踩出来的):
#
#   install_gate_service.sh 装到 /etc 的 3 个 unit 和 logrotate 规则, 是
#   【按 GATE_LOG_DIR 重写过路径】的 —— 因为 .service 里写死了
#   StandardOutput=append:/var/log/rk-gate.log 这样的绝对路径。
#
#   而 acceptance_checklist.sh 的【3】项要拿 /etc 的这些文件与【仓库模板】
#   比对, 确认"装出去的和仓库里的是同一份"。
#
#   如果这两边各写一份 sed 规则, 迟早会漂移(改了一边忘了另一边):
#     · 漂移成"永远打红" —— 验收失去意义, 每次都要人工绕过;
#     · 漂移成"永远打绿" —— 更糟: /etc 里的东西早就不是仓库里的了, 验收却看不出来。
#   所以把【唯一的那条重写规则】抽出来放在这里, 两边都 source 它。
#
# ★ 规则必须与 install_gate_service.sh 的历史行为【逐字节一致】:
#       sed "s#/var/log/#${LOG_DIR}/#g"
#   LOG_DIR=/var/log 时该替换是【恒等变换】, 所以原来的
#   "if /var/log 就 cp, 否则 sed" 两个分支可以统一成一条 ——
#   已实测: 4 个模板 sed(恒等) 与 cp 逐字节相同(见 2026-11-23 的核对)。
#   (前提: 模板文件以换行结尾; 4 个模板实测都是。sed 会给缺尾换行的文件
#    补一个, cp 不会 —— 那种情况下两条分支就会差一个字节。)
#
# 用法:
#   . scripts/logpath_render.sh
#   render_log_paths <模板文件> <日志目录> <输出文件>
#   例: render_log_paths scripts/systemd/rk-gate.service /var/log.hdd /tmp/x
# ============================================================================

render_log_paths() {
    local src="${1:-}" log_dir="${2:-}" dst="${3:-}"
    if [ -z "$src" ] || [ -z "$log_dir" ] || [ -z "$dst" ]; then
        echo "render_log_paths: 参数不足 (用法: render_log_paths <模板> <日志目录> <输出>)" >&2
        return 2
    fi
    if [ ! -r "$src" ]; then
        echo "render_log_paths: 读不到模板 $src" >&2
        return 2
    fi
    sed "s#/var/log/#${log_dir}/#g" "$src" > "$dst"
}
