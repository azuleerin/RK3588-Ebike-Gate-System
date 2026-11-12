#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""py_reference.py —— 用 speed_demo.py 的【真实实现】生成对拍基准。

直接 import speed_demo.py, 用它的 GroundPlane / BevView / is_bad_box /
SpeedMeter 跑同一组输入, 输出格式与 tests/test_speed.cpp 完全一致,
然后 diff 两份输出即可验证板端 C++ 移植是否与 Python 基线一致。

用法:
    python3 py_reference.py [speed_demo.py 的路径] > out_py.txt
"""
import importlib.util
import math
import os
import sys
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
SD_PATH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "speed_demo.py")
SD_PATH = os.path.abspath(SD_PATH)

spec = importlib.util.spec_from_file_location("sd", SD_PATH)
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)

W, H = 1920, 1080
QUAD = [(300.0, 622.0), (1500.0, 643.0), (1450.0, 877.0), (550.0, 819.0)]
FOV = 84.0
WIDTH_M = 7.0
PPM = 100.0

PROBES = [
    (300.0, 622.0), (1500.0, 643.0), (1450.0, 877.0), (550.0, 819.0),
    (900.0, 730.0), (200.0, 400.0), (1800.0, 950.0), (960.0, 300.0),
]

BOX_CASES = [
    (100, 100, 200, 300, 0),
    (0, 0, 1900, 1000, 1),
    (10, 900, 1800, 1050, 1),
    (100, 100, 1400, 200, 1),
    (900, 100, 1100, 1000, 0),
    (800, 400, 810, 405, 0),
    (800, 400, 802, 402, 1),
    (0, 0, 1300, 1080, 1),
]

MED_VALS = [100.0, 105.0, 3.0, 102.0, 98.0, 110.0, 101.0]


# ---------------------------------------------------------------- 1) IPM
focal = (W / 2.0) / math.tan(math.radians(FOV / 2.0))
gp = sd.GroundPlane(QUAD, (W, H), None, focal=focal)
print(f"F {gp.f:.10f}")
print(f"LEN_NORM {gp.len_norm:.10f}")
print(f"WIDTH_NORM {gp.width_norm:.10f}")
print(f"RECT_ERR {gp.rect_err:.10f}")

scale_c = WIDTH_M / gp.width_norm
bev = sd.BevView(QUAD, (500, 2000), PPM, ground=gp, scale_c=scale_c)
print(f"DST {bev.dst_w} {bev.dst_h}")
print(f"WIDTH_M {bev.width_m:.10f}")
print(f"LENGTH_M {bev.length_m:.10f}")

for (x, y) in PROBES:
    bx, by = bev.project([x, y])[0]
    print(f"PROJ {x:.4f} {y:.4f} {bx:.6f} {by:.6f} {1 if bev.contains((x, y)) else 0}")

# ---------------------------------------------------------------- 2) 矩形修正
fixed, v1, v2, _f = sd.correct_quad_to_rectangle(QUAD, focal, (W, H), None)
print("FIXED" + "".join(f" {p[0]:.6f} {p[1]:.6f}" for p in fixed))
print(f"V1 {v1[0]:.6f} {v1[1]:.6f}")
print(f"V2 {v2[0]:.6f} {v2[1]:.6f}")

# ---------------------------------------------------------------- 3) 几何过滤
for (x1, y1, x2, y2, _exp) in BOX_CASES:
    print(f"BAD {1 if sd.is_bad_box(x1, y1, x2, y2, W, H) else 0} {_exp}")

# ---------------------------------------------------------------- 4) 中值滤波
gpts = deque(maxlen=5)
for v in MED_VALS:
    gpts.append((v, v))
    xs = sorted(p[0] for p in gpts)
    n = len(gpts)
    print(f"MED {xs[n // 2]:.4f}")

# ---------------------------------------------------------------- 5) 跟踪 + 测速
# 单点比例尺: p1=(0,0) p2=(1000,0) = 10 m -> 0.01 m/px
# 目标每帧右移 50 px, 10 fps -> 0.5 m/s = 18 km/h
p1, p2 = (0.0, 0.0), (3000.0, 0.0)
meter = sd.SpeedMeter(p1, p2, 30.0, 0.80, bev=None, estimator="chord")

hist = deque(maxlen=240)      # (t, x, y) 原图地面接地点
gpts = deque(maxlen=5)        # 地面点中值
speeds = []
for k in range(40):
    t = k * 0.1
    x = 100 + k * 10
    gx = x + 100.0             # 框底中心 (框宽 200)
    gy = 700.0
    gpts.append((gx, gy))
    xs = sorted(p[0] for p in gpts)
    ys = sorted(p[1] for p in gpts)
    n = len(gpts)
    sx, sy = xs[n // 2], ys[n // 2]

    hist.append((t, sx, sy))
    while hist and t - hist[0][0] > meter.window_sec:
        hist.popleft()

    v = meter.speed_from_hist(list(hist), use_bev=False)
    if v is not None:
        speeds.append((t, v))
    if k >= 30 and speeds:
        # 显示值 = 最近 0.8s 窗口内中位数
        vals = sorted(vv for (tt, vv) in speeds if tt >= t - 0.8)
        if vals:
            print(f"SPEED {vals[len(vals) // 2]:.6f} ID 0")
