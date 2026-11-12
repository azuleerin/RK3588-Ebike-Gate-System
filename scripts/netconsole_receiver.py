#!/usr/bin/env python3
"""netconsole_receiver.py —— 工作站端的 netconsole 接收器

在局域网另一台机器上运行，接收 RK3588 板子通过 netconsole 发来的内核日志。

用途: 板子硬挂死时 SSH 已经断连，但 UDP 通道通常还通。这份实时日志是
      "死机前最后几秒内核输出"的唯一实时来源（ramoops 只有事后 96KB 缓冲）。

用法:
    python3 netconsole_receiver.py [端口] [输出文件]

默认端口 4444（与板端 setup_netconsole.sh 一致），输出到 kernel_netconsole.log。

仅用标准库，无需 pip 安装。
"""

import datetime
import socket
import sys

DEFAULT_PORT = 4444
DEFAULT_OUT = "kernel_netconsole.log"


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    out_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # 允许内核发送较大的报文；不设 SO_REUSEADDR 也能收
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(("0.0.0.0", port))
    sock.settimeout(1.0)

    print(f"[receiver] 监听 UDP 0.0.0.0:{port} -> {out_path}")
    print("[receiver] 在板端用 dmesg -w 或触发一次内核活动验证；Ctrl-C 退出")
    print("[receiver] 日志文件每条即时 flush 并 fdatasync，掉电也不丢\n")

    packets = 0
    with open(out_path, "a", buffering=1, encoding="utf-8", errors="replace") as fh:
        try:
            while True:
                try:
                    data, addr = sock.recvfrom(65535)
                except socket.timeout:
                    continue
                packets += 1
                ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                text = data.decode("utf-8", errors="replace").rstrip("\n")
                fh.write(f"[{ts}][{addr[0]}] {text}\n")
                fh.flush()
                # 关键：内核日志是取证材料，务必落盘（掉电不丢）
                import os
                os.fsync(fh.fileno())
                print(f"[{ts}] {text}")
        except KeyboardInterrupt:
            pass
        finally:
            sock.close()
    print(f"\n[receiver] 共收到 {packets} 个报文，日志在 {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
