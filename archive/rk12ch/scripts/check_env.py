#!/usr/bin/env python3
"""环境自检: 跑之前先把依赖、模型、硬件、零拷贝链路逐项验一遍

用法: python3 scripts/check_env.py --config config/system.yaml
"""
from __future__ import annotations

import argparse
import ctypes
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

OK, WARN, FAIL = "[ OK ]", "[WARN]", "[FAIL]"
_fails = 0


def check(cond: bool, msg: str, detail: str = "") -> bool:
    global _fails
    if cond:
        print(f"{OK} {msg}" + (f"  {detail}" if detail else ""))
    else:
        print(f"{FAIL} {msg}" + (f"  {detail}" if detail else ""))
        _fails += 1
    return cond


def check_lib(name: str) -> bool:
    try:
        ctypes.CDLL(name)
        return True
    except OSError as e:
        print(f"{FAIL} 缺少动态库 {name}: {e}")
        return False


def main() -> int:
    global _fails
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config/system.yaml")
    args = ap.parse_args()

    print("=" * 70)
    print(" RK3588 12路流水线 环境自检")
    print("=" * 70)

    print("\n[1] 动态库")
    check_lib("librknnrt.so")
    check_lib("librga.so")
    check_lib("librockchip_mpp.so")

    print("\n[2] Python 依赖")
    for mod in ("numpy", "yaml"):
        try:
            __import__(mod)
            print(f"{OK} python3-{mod}")
        except ImportError:
            print(f"{FAIL} 缺少 python3-{mod}")
            _fails += 1

    print("\n[3] 设备节点")
    for dev in ("/dev/rga", "/dev/dri", "/dev/rknpu", "/dev/mpp_service",
                "/dev/dma_heap"):
        check(os.path.exists(dev), f"设备 {dev}",
              "" if os.path.exists(dev) else "不存在")

    print("\n[4] MPP 硬解前置条件")
    import grp
    import pwd
    try:
        uid = os.getuid()
        user = pwd.getpwuid(uid).pw_name
        groups = [g.gr_name for g in grp.getgrall() if user in g.gr_mem]
        has_video = "video" in groups
    except Exception:
        user, has_video = "?", False
    st = None
    try:
        st = os.stat("/dev/mpp_service")
    except OSError:
        pass
    check(st is not None, "/dev/mpp_service 存在")
    check(has_video, f"当前用户({user})在 video 组",
          "" if has_video else "-> sudo usermod -aG video " + str(user))
    # 内核模块: MPP 报 "driver is not ready" 基本都是 rkvdec 没加载
    vpu_mod = False
    try:
        with open("/proc/modules") as f:
            vpu_mod = any("vpu" in ln or "rkvdec" in ln or "rkvenc" in ln
                          for ln in f.read().splitlines())
    except OSError:
        pass
    check(vpu_mod, "VPU 内核模块已加载(rkvdec)",
          "" if vpu_mod else "-> 没加载时 MPP 会报 'driver is not ready', "
                             "MPP 硬解不可用(只能软解)")

    print("\n[5] dma-heap")
    heaps = ["/dev/dma_heap/rk3588-system", "/dev/dma_heap/system",
             "/dev/dma_heap/rknpu"]
    found = [h for h in heaps if os.path.exists(h)]
    check(bool(found), "可用 dma-heap", ", ".join(found) if found else "无")

    print("\n[6] dma-buf 分配")
    try:
        from rk12ch.hal.dmabuf import alloc_dmabuf
        buf = alloc_dmabuf(1 << 20)
        if buf is not None and buf.fd >= 0:
            print(f"{OK} dma-buf 分配成功 fd={buf.fd}")
            buf.close()
        else:
            print(f"{WARN} dma-buf 分配失败 -> RGA 零拷贝不可用, 会退化成 CPU 路径")
    except Exception as e:
        print(f"{WARN} dma-buf 自检异常: {e}")

    print("\n[7] 模型")
    try:
        from rk12ch.config import load_config
        cfg = load_config(args.config)
        print(f"{OK} 配置校验通过: {len(cfg['channels'])} 路")
    except Exception as e:
        print(f"{FAIL} 配置/模型校验失败: {e}")
        _fails += 1
        cfg = None

    print("\n[8] RKNN 模型属性")
    if cfg:
        try:
            from rk12ch.hal.rknn import RknnModel
            for key in ("yolo", "lpr"):
                m = RknnModel(cfg["models"][key]["path"])
                print(f"{OK} {key}: 输入={[(i.name, i.shape, i.type_name) for i in m.info.inputs]}")
                print(f"     输出={[(o.name, o.shape) for o in m.info.outputs]}")
                bd = m.info.inputs[0].dims[0] if m.info.inputs[0].dims else 1
                if bd <= 1:
                    print(f"{WARN} {key} 的 batch 维是 1 -> batch 推理不生效,"
                          f"需要用 rknn.config(batch_size=N) 重新转换")
                m.close()
        except Exception as e:
            print(f"{FAIL} RKNN 模型加载失败: {e}")
            _fails += 1

    print("\n" + "=" * 70)
    print(f" 结果: {_fails} 项失败")
    print("=" * 70)
    return 1 if _fails else 0


if __name__ == "__main__":
    sys.exit(main())
