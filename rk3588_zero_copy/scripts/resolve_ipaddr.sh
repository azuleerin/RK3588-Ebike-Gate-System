#!/usr/bin/env bash
# ============================================================================
# resolve_ipaddr.sh —— GATE_IPADDR 的解析逻辑(生产与沙盒渲染共用同一份)
#
# ★ 为什么要抽成共享文件(2026-11-24):
#   解析逻辑原来只写在 run_gate_service.sh 里, 而沙盒渲染命令行走的是
#   opt_ebike_fork9/tools/render_gate_args.sh —— 两边各写一份必然漂移:
#   渲染出来的是字面量 "auto", 生产跑出来的是真实 IP, 冒烟测的就不是同一个东西。
#   现在只有这一份, 两边都 source 它。
#
# 用法:  . resolve_ipaddr.sh ; GATE_IPADDR_RESOLVED=$(resolve_ipaddr "$GATE_IPADDR")
#
# ★ 为什么用 auto 而不是写死一个 IP(R12):
#   现场网卡地址会变(换网段/换路由器/DHCP 续租), 写死的 IP 过期后
#   【不会报错】, 只是上报给后端的地址是错的 —— 现场表现为
#   "设备在线但数据看不到", 极难定位。
#   实测踩过: gate.env 写 192.168.31.77, 而板子早已换到 192.168.8.77。
#
# 取值顺序:
#   1) ip route get <对外地址> 的 src —— 最准, 就是"默认路由会用的那个源地址"
#   2) ip -4 -o addr show scope global 的第一个非回环地址
#   3) 都拿不到 -> 回退 127.0.0.1 并打 WARN(明确告诉人, 不静默)
# ============================================================================

resolve_ipaddr() {
    local want="${1:-auto}" ip=""

    if [ "$want" != "auto" ] && [ -n "$want" ]; then
        printf '%s' "$want"
        return 0
    fi

    ip=$(ip -4 route get 1.1.1.1 2>/dev/null |
         sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -1)
    if [ -z "$ip" ]; then
        ip=$(ip -4 -o addr show scope global 2>/dev/null |
             awk '{split($4,a,"/"); if (a[1] != "127.0.0.1") {print a[1]; exit}}')
    fi
    if [ -z "$ip" ]; then
        echo "[gate] ! GATE_IPADDR=auto 但取不到本机 IPv4 —— 回退 127.0.0.1(请人工确认网口)" >&2
        printf '%s' "127.0.0.1"
        return 0
    fi
    printf '%s' "$ip"
}
