#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
draw_ipm_points.py —— RK3588 测速模块 IPM 标定点的现场选取工具

======================================================================
用途
======================================================================
现场用相机截图上点出「路面上那个矩形区域」的 4 个角, 工具自动做两件事:

  1) 把用户乱点的 4 个点纠正成板端要求的环形顺序
     P0 = 远端左, P1 = 远端右, P2 = 近端右, P3 = 近端左
  2) 把截图里的像素坐标换算到「流水线实际帧分辨率」的坐标空间

最后打印一段可以直接复制粘贴到板端启动命令里的 --ipm-points 参数。

======================================================================
和板端参数的对应关系
======================================================================
板端:
  rk3588_zero_copy/src/main.cc   --ipm-points "x,y;x,y;x,y;x,y"   (标定梯形 4 点, 图像坐标)
  rk3588_zero_copy/src/ipm.cc     GroundPlane::solve(quad, img_w, img_h, ...)
                                  其中 line_intersect(quad[0],quad[1],quad[2],quad[3])
                                       与 line_intersect(quad[1],quad[2],quad[3],quad[0])
                                  求两个灭点。
=> 4 点必须是矩形在图上的「环形顶点序列」, 一旦交叉(打花)就算不出正确的焦距。
=> 坐标空间 = 解码出来的原始视频帧的像素坐标(y 向下增大), 不是显示分辨率。

======================================================================
点序语义(工具输出的顺序, 也是板端需要的顺序)
======================================================================
  P0 远端左   ┌─────────────────┐  远边 (y 小, 离相机远)
  P1 远端右   │                 │
  P2 近端右   └─────────────────┘  近边 (y 大, 离相机近)
  P3 近端左
即: 先点远端那条边的 左→右, 再点近端那条边的 右→左。
用户可以随便乱点, 工具会自动纠正到上面这个顺序, 并在图上用不同颜色
标出「远边 / 近边」, 一眼就能看出顺序对不对。

======================================================================
用法示例
======================================================================
  # 1) 交互选点(在有显示器的笔记本上跑)
  python3 tools/draw_ipm_points.py shot.jpg
  python3 tools/draw_ipm_points.py shot.jpg --size 1920x1080 --out /tmp/ipm.txt

  # 2) 无界面自测(只跑「顺序纠正 + 分辨率换算」的纯逻辑, 不开窗口)
  python3 tools/draw_ipm_points.py --selftest "300,622;1500,643;1450,877;550,819"
  python3 tools/draw_ipm_points.py --selftest "600,1244;3000,1286;2900,1754;1100,1638" \
      --src-size 3840x2160 --size 1920x1080

  # 3) 无窗口模拟点击(跑完整条交互链路: 显示缩放 -> 原图 -> 流水线 -> 纠正)
  python3 tools/draw_ipm_points.py shot.jpg --simulate "300,622;1500,643;1450,877;550,819"

  # 板端启动命令用法:
  ./run_demo.sh --ipm-points "300,622;1500,643;1450,877;550,819" ...

======================================================================
操作说明
======================================================================
  鼠标左键 : 依次点 4 个角(顺序随意, 工具会纠正)
  r         : 重新选点
  Esc / q   : 退出(关闭窗口也可以)
  Ctrl+C    : 强制退出

依赖: 只用 cv2 / numpy / argparse / sys / os(site 仅在 import 兜底时用一次),
      没有 matplotlib 之类的东西。
"""

import argparse
import os
import sys

try:
    import cv2
    import numpy as np
except ImportError:
    # 板端把 opencv 装在用户 site-packages 里, 而 `python3 -I`(隔离模式)默认
    # 不把用户 site-packages 加进 sys.path, 这里手动补一次, 保证板端也能用
    # -I 跑自测。注意是 append 到末尾: 脚本目录/当前目录里的同名模块仍然优先,
    # 不会因为这一步就放松隔离。
    import site as _site
    _user_site = _site.getusersitepackages()
    if _user_site and os.path.isdir(_user_site) and _user_site not in sys.path:
        sys.path.append(_user_site)
    import cv2
    import numpy as np

# ---------------------------------------------------------------- 常量

WINDOW_NAME = "IPM point picker"

# 窗口最大显示尺寸(超过就按比例缩小显示)。板子上没有显示器, 但这个工具
# 是给用户在自己的笔记本上用的, 所以这里给一个保守的上限即可。
MAX_DISPLAY_W = 1600
MAX_DISPLAY_H = 900

# 颜色一律用 OpenCV 的 BGR 顺序
CLR_FAR = (0, 0, 255)      # 远边 = 红
CLR_NEAR = (0, 255, 0)     # 近边 = 绿
CLR_SIDE = (0, 255, 255)   # 两条侧边 = 黄
CLR_PT = (255, 255, 255)   # 选点标记 = 白
CLR_TXT = (255, 255, 255)

# 点序的语义标签(注意: cv2.putText 只能画 ASCII, 画不了中文, 所以图上的
# 文字用英文缩写, 中文说明一律在终端里打印)
LABELS = ("P0 far-L", "P1 far-R", "P2 near-R", "P3 near-L")
LABELS_CN = ("P0 远端左", "P1 远端右", "P2 近端右", "P3 近端左")


# ---------------------------------------------------------------- 基础工具

def eprint(msg):
    """错误信息统一走 stderr, 方便脚本判断。"""
    try:
        sys.stdout.flush()
    except Exception:
        pass
    print(msg, file=sys.stderr)
    try:
        sys.stderr.flush()
    except Exception:
        pass


def die(msg, code=1):
    eprint("[错误] " + msg)
    sys.exit(code)


def safe_destroy_windows():
    """退出前销毁窗口。

    注意: opencv-python-headless 这类没有 highgui GUI 支持的编译版本,
    连 destroyAllWindows() 自己都会抛 cv2.error, 所以这里必须兜住,
    否则 headless 环境会打印一串没用的 traceback, 把真正的中文提示冲掉。
    """
    try:
        cv2.destroyAllWindows()
    except Exception:
        pass


def parse_size(text):
    """把 '1920x1080' 解析成 (1920, 1080)。"""
    t = str(text).lower().replace("*", "x").replace("×", "x").strip()
    if "x" not in t:
        raise ValueError("分辨率格式应为 WxH, 例如 1920x1080")
    a, b = t.split("x", 1)
    try:
        w, h = int(a.strip()), int(b.strip())
    except ValueError:
        raise ValueError("分辨率格式应为 WxH, 例如 1920x1080")
    if w <= 0 or h <= 0:
        raise ValueError("分辨率必须为正数")
    return w, h


def parse_points(text):
    """把 'x,y;x,y;x,y;x,y' 解析成 4 个 (x, y) 浮点元组。"""
    pts = []
    for chunk in str(text).split(";"):
        chunk = chunk.strip()
        if not chunk:
            continue
        if "," not in chunk:
            raise ValueError("点的格式应为 x,y")
        xs, ys = chunk.split(",", 1)
        try:
            pts.append((float(xs.strip()), float(ys.strip())))
        except ValueError:
            raise ValueError("点的格式应为 x,y(数字), 实际读到 '%s'" % chunk)
    if len(pts) != 4:
        raise ValueError("必须正好 4 个点, 实际读到 %d 个" % len(pts))
    return pts


def round_half_up(v):
    """四舍五入(不是 Python round 的「银行家舍入」), 并且不依赖 math 模块。"""
    v = float(v)
    return int(v + 0.5) if v >= 0 else -int(-v + 0.5)


def fmt_points(pts):
    """输出成板端要的 'x,y;x,y;x,y;x,y' 格式(整数, 四舍五入)。"""
    return ";".join("%d,%d" % (round_half_up(p[0]), round_half_up(p[1])) for p in pts)


def disp_width(s):
    """终端里一个中文字符占 2 列, 用来对齐表格。"""
    w = 0
    for ch in s:
        if ("一" <= ch <= "鿿") or ("　" <= ch <= "〿") or ("＀" <= ch <= "￯"):
            w += 2
        else:
            w += 1
    return w


def pad_cjk(s, width):
    """按显示宽度左对齐补空格(中文字符算 2 列)。"""
    return s + " " * max(0, width - disp_width(s))


def scale_points(pts, sx, sy):
    """按比例把点从原图坐标空间换算到流水线坐标空间。

    为什么必须换算: 板端 GroundPlane::solve() 吃的是「解码出来的原始视频帧」
    的像素坐标(nv.width/nv.height), 不是用户手里那张截图的分辨率。
    如果截图是 3840x2160 而流水线是 1920x1080, 不除以 2 的话板端拿到的
    角点会跑到画面外面, 焦距直接算错。
    """
    return [(p[0] * sx, p[1] * sy) for p in pts]


def seg_cross(p1, p2, p3, p4):
    """两条线段是否真交叉(只用来报警, 不参与主流程判定)。"""
    def ccw(a, b, c):
        return (c[1] - a[1]) * (b[0] - a[0]) > (b[1] - a[1]) * (c[0] - a[0])
    d1 = ccw(p1, p2, p3)
    d2 = ccw(p1, p2, p4)
    d3 = ccw(p3, p4, p1)
    d4 = ccw(p3, p4, p2)
    return d1 != d2 and d3 != d4


def is_simple_quad(pts):
    """4 点是否构成不自交的菱形(环形顶点序列)。"""
    return not seg_cross(pts[0], pts[1], pts[2], pts[3]) and \
           not seg_cross(pts[1], pts[2], pts[3], pts[0])


# ---------------------------------------------------------------- 核心: 顺序纠正

def order_quad(pts):
    """把任意点序的 4 个点纠正成 P0 远端左 / P1 远端右 / P2 近端右 / P3 近端左。

    返回 (ordered_pts, order_idx, info)
      ordered_pts : 纠正后的 4 个点(坐标值)
      order_idx   : 纠正后的点对应的「原始点击序号」0..3
      info        : 判定过程, 用来在终端里解释为什么这么排

    ------------------------------------------------------------------
    为什么这么判断「远 / 近」:
      相机是俯视路面的, 所以越远的路面在图像里越靠上(y 越小), 近处 y 大。
      4 个点两两配对只有 3 种方式:
          (0,1)+(2,3), (0,2)+(1,3), (0,3)+(1,2)
      其中只有一种能把「远边」和「近边」正确分开, 判据是: 这两条边的
      平均 y 之差(|Δy|)最大。把这一对里 y 平均值小的叫远边(top),
      y 平均值大的叫近边(bottom)。

      排第二的判据是「远边更短」: 俯视路面时远处那条边在图上投影更短,
      当两种配对的 |Δy| 打平时, 取远边更短的那个。

      接下来才是左右: 远边按 x 从小到大 = 左→右 = P0, P1;
      近边按 x 从小到大 = 左→右, 但板端要求输出顺序是「先右后左」,
      所以 P2 = 近边右点, P3 = 近边左点。
      这样得到的序列 P0→P1→P2→P3 沿着矩形走一圈, 一定是环形的, 不交叉。
    """
    pairings = (((0, 1), (2, 3)), ((0, 2), (1, 3)), ((0, 3), (1, 2)))

    cands = []
    for pa, pb in pairings:
        ya = 0.5 * (pts[pa[0]][1] + pts[pa[1]][1])
        yb = 0.5 * (pts[pb[0]][1] + pts[pb[1]][1])
        diff = abs(ya - yb)
        # y 小的那对 = 远边(top)
        if ya <= yb:
            top, bot = pa, pb
        else:
            top, bot = pb, pa
        w_top = abs(pts[top[0]][0] - pts[top[1]][0])
        cands.append((diff, w_top, top, bot))

    # 主排序 |Δy| 越大越好, 次排序远边越短越好
    cands.sort(key=lambda c: (-c[0], c[1]))
    diff, w_top, top, bot = cands[0]

    # 远边: x 从小到大 -> 左, 右
    far = sorted(top, key=lambda i: pts[i][0])
    # 近边: x 从小到大 -> 左, 右 (输出时先右后左)
    near = sorted(bot, key=lambda i: pts[i][0])

    order_idx = [far[0], far[1], near[1], near[0]]
    ordered = [pts[i] for i in order_idx]

    info = {
        "diff": diff,
        "far_idx": tuple(far),
        "near_idx": tuple(near),
        "far_width": w_top,
        "changed": order_idx != [0, 1, 2, 3],
        "simple": is_simple_quad(ordered),
    }
    return ordered, order_idx, info


# ---------------------------------------------------------------- 自测路径

def run_selftest(args, pipe_w, pipe_h):
    """无界面自测: 只跑「顺序纠正 + 分辨率换算」的纯逻辑, 方便无头验证。"""
    try:
        pts = parse_points(args.selftest)
    except ValueError as e:
        die("--selftest 参数无效: %s" % e)

    # 自测的「原图分辨率」: 没给 --src-size 时认为输入点已经在流水线空间里
    if args.src_size:
        try:
            src_w, src_h = parse_size(args.src_size)
        except ValueError as e:
            die("--src-size 无效: %s" % e)
    else:
        src_w, src_h = pipe_w, pipe_h

    sx = pipe_w / float(src_w)
    sy = pipe_h / float(src_h)

    ordered, order_idx, info = order_quad(pts)
    scaled = scale_points(ordered, sx, sy)

    print("[自测] 输入(原图 %dx%d): %s" % (src_w, src_h, fmt_points(pts)))
    if info["changed"]:
        print("[自测] 顺序纠正: 已纠正 %s -> %s" % (
            ",".join(str(i + 1) for i in range(4)),
            ",".join(str(i + 1) for i in order_idx)))
    else:
        print("[自测] 顺序纠正: 保持不变 (远边=点%d+点%d, 近边=点%d+点%d)" % (
            info["far_idx"][0] + 1, info["far_idx"][1] + 1,
            info["near_idx"][0] + 1, info["near_idx"][1] + 1))
    print("[自测] 远/近判据 |Δy|=%.1f, 远边宽=%d px" % (info["diff"], info["far_width"]))
    print("[自测] 换算: 原图 %dx%d -> 流水线 %dx%d, 比例 x=%.6f y=%.6f" % (
        src_w, src_h, pipe_w, pipe_h, sx, sy))
    print("[自测] 输出(流水线 %dx%d): %s" % (pipe_w, pipe_h, fmt_points(scaled)))
    if not info["simple"]:
        print("[自测] [警告] 纠正后的 4 点是交叉的, 板端会算错焦距, 请重新选点")
    print("--ipm-points \"%s\"" % fmt_points(scaled))
    print("[自测] OK")
    return 0


# ---------------------------------------------------------------- 读图

def imread_any(path):
    """读图, 顺便兼容带中文/非 ASCII 的文件名(np.fromfile + imdecode)。"""
    try:
        data = np.fromfile(path, dtype=np.uint8)
        if data.size:
            img = cv2.imdecode(data, cv2.IMREAD_COLOR)
            if img is not None:
                return img
    except Exception:
        pass
    return cv2.imread(path, cv2.IMREAD_COLOR)


def build_result_block(pipe_w, pipe_h, src_w, src_h, sx, sy, pts, order_idx):
    """生成结果文本(终端和 --out 文件共用)。"""
    lines = []
    lines.append("================ IPM 标定结果 ================")
    lines.append('--ipm-points "%s"' % fmt_points(pts))
    lines.append("")
    lines.append("点序: P0=远端左  P1=远端右  P2=近端右  P3=近端左")
    for k in range(4):
        lines.append("  %s(%d, %d)   [点击序号 %d]" % (
            pad_cjk(LABELS_CN[k], 12), round_half_up(pts[k][0]), round_half_up(pts[k][1]),
            order_idx[k] + 1))
    lines.append("")
    lines.append("换算说明:")
    lines.append("  截图(原图)分辨率 : %dx%d" % (src_w, src_h))
    lines.append("  流水线帧分辨率   : %dx%d" % (pipe_w, pipe_h))
    lines.append("  缩放系数         : x=%.6f, y=%.6f   (流水线坐标 = 截图坐标 * 系数)" % (sx, sy))
    lines.append("  坐标空间         : 流水线原始帧像素坐标(y 向下增大), "
                  "板端 GroundPlane::solve() 用的就是这个空间")
    lines.append("==============================================")
    return "\n".join(lines)


# ---------------------------------------------------------------- 交互核心

class Picker(object):
    """IPM 选点会话: 负责「显示坐标 <-> 原图坐标 <-> 流水线坐标」的换算,
    以及 4 点选完后的顺序纠正和画图。

    单独抽成类是为了让交互逻辑(点选/换算/纠正/渲染)可以脱离 highgui 单独
    跑, 这样 --simulate 就能在板子上无头验证整条链路。
    """

    def __init__(self, img, pipe_w, pipe_h):
        self.img = img
        self.src_h, self.src_w = img.shape[:2]
        self.pipe_w = pipe_w
        self.pipe_h = pipe_h

        # 截图分辨率 -> 流水线帧分辨率的换算系数。
        # 板端 GroundPlane::solve() 吃的是原始帧的像素坐标, 截图可能是 4K,
        # 所以必须换算, 否则板端拿到的角点会跑到画面外面。
        self.sx = pipe_w / float(self.src_w)
        self.sy = pipe_h / float(self.src_h)

        # 显示缩放: 图片比窗口大就缩小显示, 但要记住缩放系数,
        # 鼠标点到的显示坐标要换算回原图坐标再画/再输出。
        self.disp_ratio = min(1.0, MAX_DISPLAY_W / float(self.src_w),
                              MAX_DISPLAY_H / float(self.src_h))
        self.disp_w = max(1, int(round(self.src_w * self.disp_ratio)))
        self.disp_h = max(1, int(round(self.src_h * self.disp_ratio)))
        # 用「实际显示尺寸 / 原图尺寸」换算, 避免 round 带来的 1px 累积误差
        self.d2sx = self.disp_w / float(self.src_w)
        self.d2sy = self.disp_h / float(self.src_h)

        self.font_scale = max(0.4, min(1.0, self.disp_w / 1600.0))
        self.thickness = max(1, int(self.disp_w / 900))
        self.radius = max(5, int(self.disp_w / 160))

        self.pts = []        # 已经点过的点(原图坐标)
        self.ordered = None  # 纠正顺序后的点(流水线坐标)
        self.order_idx = None
        self.info = None

    # ---- 坐标换算 ----

    def to_pipeline(self, p):
        """原图坐标 -> 流水线坐标。"""
        return (p[0] * self.sx, p[1] * self.sy)

    def to_display(self, p):
        """原图坐标 -> 显示坐标(画图用)。"""
        return (p[0] * self.d2sx, p[1] * self.d2sy)

    def add_click(self, disp_x, disp_y):
        """处理一次鼠标左键点击(参数是显示图上的坐标)。"""
        if len(self.pts) >= 4:
            return False
        # 显示坐标 -> 原图坐标
        ox = disp_x / self.d2sx
        oy = disp_y / self.d2sy
        self.pts.append((ox, oy))
        if len(self.pts) == 4:
            # 在流水线坐标系里纠正顺序(语义空间和最终输出一致)
            pipe_pts = scale_points(self.pts, self.sx, self.sy)
            self.ordered, self.order_idx, self.info = order_quad(pipe_pts)
        return True

    def reset(self):
        self.pts = []
        self.ordered = None
        self.order_idx = None
        self.info = None

    # ---- 画图 ----

    def _text(self, imgd, text, org, color=CLR_TXT, scale=None, thick=None):
        cv2.putText(imgd, text, org, cv2.FONT_HERSHEY_SIMPLEX,
                    self.font_scale if scale is None else scale,
                    color, self.thickness if thick is None else thick, cv2.LINE_AA)

    def render(self):
        """把原图缩放到显示尺寸, 再把标记和纠正后的顺序画上去。"""
        interp = cv2.INTER_AREA if self.disp_ratio < 1.0 else cv2.INTER_LINEAR
        disp = cv2.resize(self.img, (self.disp_w, self.disp_h), interpolation=interp)

        # --- 已选的点(按点击顺序画 1..N) ---
        for n, p in enumerate(self.pts):
            cx = int(round(p[0] * self.d2sx))
            cy = int(round(p[1] * self.d2sy))
            cv2.circle(disp, (cx, cy), self.radius, CLR_PT, self.thickness, cv2.LINE_AA)
            cv2.circle(disp, (cx, cy), self.radius + 2, (60, 60, 60), 1, cv2.LINE_AA)
            self._text(disp, str(n + 1), (cx + self.radius + 3, cy - self.radius - 3))

        # --- 纠正后的顺序: 远边红 / 近边绿 / 侧边黄, 并标 P0..P3 ---
        if self.order_idx:
            # 流水线坐标 -> 原图坐标 -> 显示坐标, 用来画线
            back = [(self.ordered[k][0] / self.sx, self.ordered[k][1] / self.sy)
                    for k in range(4)]
            dpts = [(int(round(q[0] * self.d2sx)), int(round(q[1] * self.d2sy)))
                    for q in back]
            cv2.line(disp, dpts[0], dpts[1], CLR_FAR, self.thickness + 2, cv2.LINE_AA)
            cv2.line(disp, dpts[2], dpts[3], CLR_NEAR, self.thickness + 2, cv2.LINE_AA)
            cv2.line(disp, dpts[1], dpts[2], CLR_SIDE, self.thickness, cv2.LINE_AA)
            cv2.line(disp, dpts[3], dpts[0], CLR_SIDE, self.thickness, cv2.LINE_AA)
            for k in range(4):
                cv2.circle(disp, dpts[k], self.radius + 3,
                           CLR_FAR if k < 2 else CLR_NEAR, 2, cv2.LINE_AA)
                self._text(disp, LABELS[k],
                           (dpts[k][0] + self.radius + 6, dpts[k][1] + self.radius + 6),
                           CLR_FAR if k < 2 else CLR_NEAR)
            self._text(disp, "far edge (red) / near edge (green)",
                       (10, self.disp_h - 12), CLR_TXT)

        # --- 顶部状态条(半透明黑条, 让文字看得清) ---
        cnt = len(self.pts)
        overlay = disp.copy()
        cv2.rectangle(overlay, (0, 0), (self.disp_w, 46), (0, 0, 0), -1)
        disp = cv2.addWeighted(overlay, 0.45, disp, 0.55, 0)
        if cnt < 4:
            self._text(disp, "clicked %d/4 corners  (r = reset, Esc/q = quit)" % cnt,
                       (10, 32))
        else:
            self._text(disp, "4/4 corners OK  (r = reset, Esc/q = quit)", (10, 32))
        return disp


def load_image(path):
    """读图, 失败时给中文报错并 exit 1。"""
    if not os.path.exists(path):
        die("找不到图片文件: %s" % path)
    if not os.path.isfile(path):
        die("这不是一个普通文件: %s" % path)
    img = imread_any(path)
    if img is None:
        die("无法读取图片(不是合法图片 / 文件已损坏): %s" % path)
    return img


def print_result(picker, args):
    """打印结果块, 并按需写进 --out 文件。"""
    if picker.ordered is None:
        return
    ordered = picker.ordered
    order_idx = picker.order_idx
    block = build_result_block(picker.pipe_w, picker.pipe_h, picker.src_w, picker.src_h,
                                picker.sx, picker.sy, ordered, order_idx)
    print("")
    print(block)
    if picker.info and not picker.info["simple"]:
        print("[警告] 纠正后的 4 点是交叉的, 板端会算错焦距, 建议按 r 重新选点")
    if args.out:
        try:
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(block + "\n")
            print("已写入: %s" % os.path.abspath(args.out))
        except Exception as e:
            eprint("[错误] 写文件失败 %s: %s" % (args.out, e))


# ---------------------------------------------------------------- GUI 主流程

def run_gui(args, pipe_w, pipe_h):
    """交互选点主流程(需要图形界面)。"""
    img = load_image(args.image)
    picker = Picker(img, pipe_w, pipe_h)

    print("图片: %s" % os.path.abspath(args.image))
    print("截图(原图)分辨率: %dx%d   流水线帧分辨率: %dx%d" %
          (picker.src_w, picker.src_h, pipe_w, pipe_h))
    print("显示缩放: %.4f (显示 %dx%d, 鼠标坐标会换算回原图)" %
          (picker.disp_ratio, picker.disp_w, picker.disp_h))
    print("请依次点击路面上那个矩形的 4 个角(顺序随意, 工具会自动纠正)。")
    print("按 r 重新选点, 按 Esc 或 q 退出。")
    print("")

    printed = [None]   # 已经打印过结果的点集, 用来判断退出前要不要补打

    def on_mouse(event, x, y, flags, param):
        if event != cv2.EVENT_LBUTTONDOWN:
            return
        if len(picker.pts) >= 4:
            print("[提示] 已经点了 4 个点, 按 r 可以重新选点")
            return
        if not picker.add_click(x, y):
            return
        n = len(picker.pts)
        ox, oy = picker.pts[-1]
        pp = picker.to_pipeline(picker.pts[-1])
        print("[选点 %d/4] 原图(%d, %d)  流水线(%d, %d)" % (
            n, round_half_up(ox), round_half_up(oy),
            round_half_up(pp[0]), round_half_up(pp[1])))
        if n == 4:
            print_result(picker, args)
            printed[0] = tuple(picker.pts)

    # --- 无图形界面的情况: 提前给一句人话, 而不是让用户看 traceback ---
    if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") \
            and not os.environ.get("WAYLAND_DISPLAY"):
        die("当前环境没有图形界面, 请在有显示器的电脑上运行"
            "(或用 --selftest 做无界面自测)")

    try:
        cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_AUTOSIZE)
        cv2.imshow(WINDOW_NAME, picker.render())
        cv2.setMouseCallback(WINDOW_NAME, on_mouse)
        while True:
            cv2.imshow(WINDOW_NAME, picker.render())
            key = cv2.waitKey(20)
            if key == -1:          # 用户直接关了窗口
                break
            key &= 0xFF
            if key == 27 or key == ord('q'):
                break
            if key == ord('r'):
                picker.reset()
                print("[重选] 已清空, 请重新点 4 个角")
    except cv2.error as e:
        die("当前环境没有图形界面, 请在有显示器的电脑上运行"
            "(或用 --selftest 做无界面自测)\n[cv2] %s" % e)
    finally:
        safe_destroy_windows()

    # 退出前如果点过又改过(比如按 r 重选过), 补打一次最终结果
    if printed[0] is not None and tuple(picker.pts) != printed[0] \
            and picker.ordered is not None:
        print_result(picker, args)
    if picker.ordered is None:
        print("[结束] 没有完成选点, 未输出标定参数。")
        return 0
    return 0


def run_simulate(args, pipe_w, pipe_h):
    """不开窗口, 用给定的原图坐标模拟 4 次点击, 跑完整条交互链路。

    用来在板子(headless)上验证「显示缩放 -> 原图 -> 流水线」的换算和
    顺序纠正。给出的坐标是原图(截图)坐标, 内部会先换算成显示坐标再走
    真正的 add_click(), 所以换算链路和真人鼠标点击完全一致。
    """
    img = load_image(args.image)
    picker = Picker(img, pipe_w, pipe_h)

    try:
        pts = parse_points(args.simulate)
    except ValueError as e:
        die("--simulate 参数无效: %s" % e)

    print("图片: %s" % os.path.abspath(args.image))
    print("截图(原图)分辨率: %dx%d   流水线帧分辨率: %dx%d" %
          (picker.src_w, picker.src_h, pipe_w, pipe_h))
    print("显示缩放: %.4f (显示 %dx%d)" % (picker.disp_ratio, picker.disp_w, picker.disp_h))
    print("[模拟] 输入(原图坐标): %s" % fmt_points(pts))
    print("")

    for i, p in enumerate(pts):
        # 原图坐标 -> 显示坐标, 再走真正的点击处理(显示 -> 原图 换算)
        dx = p[0] * picker.d2sx
        dy = p[1] * picker.d2sy
        picker.add_click(dx, dy)
        ox, oy = picker.pts[-1]
        pp = picker.to_pipeline(picker.pts[-1])
        print("[模拟 %d/4] 显示(%d, %d) -> 原图(%d, %d) -> 流水线(%d, %d)" % (
            i + 1, round_half_up(dx), round_half_up(dy),
            round_half_up(ox), round_half_up(oy),
            round_half_up(pp[0]), round_half_up(pp[1])))

    if picker.ordered is None:
        die("--simulate 需要正好 4 个点")

    # 跑一遍渲染, 保证画图代码路径没问题(headless 下 cv2 画图是可以的)
    picker.render()

    print_result(picker, args)
    return 0


# ---------------------------------------------------------------- 入口

def build_parser():
    p = argparse.ArgumentParser(
        description="RK3588 测速模块 IPM 标定点现场选取工具",
        formatter_class=argparse.RawTextHelpFormatter,
        epilog="示例:\n"
               "  python3 tools/draw_ipm_points.py shot.jpg --size 1920x1080\n"
               "  python3 tools/draw_ipm_points.py --selftest \"300,622;1500,643;1450,877;550,819\"\n")
    p.add_argument("image", nargs="?",
                   help="现场相机截图路径(--selftest 模式可省略)")
    p.add_argument("--size", default="1920x1080",
                   help="流水线实际帧分辨率, 默认 1920x1080")
    p.add_argument("--src-size", default=None,
                   help="仅 --selftest 用: 输入点所在的原图分辨率, 默认与 --size 相同")
    p.add_argument("--out", default=None,
                   help="把结果同时写进一个文件")
    p.add_argument("--selftest", default=None,
                   help="无界面自测: 对给定的 4 个点跑顺序纠正 + 分辨率换算")
    p.add_argument("--simulate", default=None,
                   help="无界面自测: 不开窗口, 用原图坐标模拟 4 次点击, "
                        "跑完整条交互链路(坐标按原图给)")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)

    try:
        pipe_w, pipe_h = parse_size(args.size)
    except ValueError as e:
        die("--size 无效: %s" % e)

    if args.selftest is not None:
        return run_selftest(args, pipe_w, pipe_h)

    if args.simulate is not None:
        if not args.image:
            die("--simulate 需要同时给图片路径")
        return run_simulate(args, pipe_w, pipe_h)

    if not args.image:
        die("缺少图片路径(用法: draw_ipm_points.py <图片> [--size WxH] [--out 文件])")

    try:
        return run_gui(args, pipe_w, pipe_h)
    except SystemExit:
        raise
    except KeyboardInterrupt:
        safe_destroy_windows()
        return 0
    except cv2.error as e:
        safe_destroy_windows()
        die("当前环境没有图形界面, 请在有显示器的电脑上运行"
            "(或用 --selftest 做无界面自测)\n[cv2] %s" % e)
    finally:
        safe_destroy_windows()


if __name__ == "__main__":
    sys.exit(main())
