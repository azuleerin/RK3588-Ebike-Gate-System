#!/usr/bin/env python3
"""生成一张合成车牌图, 给 lpr_test 做冒烟测试用。

板子上没有真实车牌照片(摄像头对着办公桌), 所以这里合成一张。

★ 合成样式是【调出来的】, 不是随手画的:
  lprnet_finetuned_int8 这个模型吃的是【白底黑字 + 字符间距很宽】的图
  (实测: 蓝底白字的整牌渲染它只认出一个字母, 白底黑字+宽间距能全认出来,
   同样的字用 FONT_HERSHEY_SIMPLEX + 0.6 字宽的间距, 置信度 0.93)。
  真实照片直接丢给 lpr_test 就行, 不用这个脚本。

用法:
    python3 tests/gen_test_plate.py -o test_plate.jpg -t B12345
    python3 tests/gen_test_plate.py -o plate.jpg -t B12345 --style blue
"""
import argparse
import cv2
import numpy as np

# 普通蓝牌配色 (BGR)
BLUE = (170, 82, 0)
WHITE = (250, 250, 250)
BLACK = (10, 10, 10)


def render_chars(text, out_w=400, out_h=120, scale=50, gap=0.4, thick=6):
    """白底黑字 + 宽间距 —— 模型最吃得下的一种

    注意: OpenCV 5 的 Hershey 字体度量比 OpenCV 4 大了 20 倍左右,
    画出来的字会非常大。所以这里【先画大, 再裁到文字外接框, 最后缩放
    到车牌尺寸贴到画布上】, 这样不管 OpenCV 版本怎么变, 出来的图尺寸都
    是一样的, 跟真实照片的比例才对得上。
    """
    font = cv2.FONT_HERSHEY_SIMPLEX
    items = []
    for ch in text:
        (w, h), _ = cv2.getTextSize(ch, font, scale, thick)
        items.append((ch, w, h))
    W = sum(w for _, w, _ in items) + int(gap * scale) * (len(items) - 1)
    H = max(h for _, _, h in items)
    pad = int(H * 0.3)
    big = np.full((H + 2 * pad, W + 2 * pad, 3), WHITE, dtype=np.uint8)

    x = pad
    for ch, w, h in items:
        cv2.putText(big, ch, (x, pad + h), font, scale, BLACK, thick, cv2.LINE_AA)
        x += w + int(gap * scale)

    # 裁到文字外接框
    gray = cv2.cvtColor(big, cv2.COLOR_BGR2GRAY)
    ys, xs = np.where(gray < 200)
    if len(xs) and len(ys):
        big = big[ys.min():ys.max() + 1, xs.min():xs.max() + 1]

    # 等比缩放到画布内(宽高都要装得下), 贴到画布中间
    bh, bw = big.shape[:2]
    s = min((out_h * 0.86) / bh, (out_w * 0.86) / bw)
    nw, nh = max(1, int(bw * s)), max(1, int(bh * s))
    small = cv2.resize(big, (nw, nh), interpolation=cv2.INTER_AREA)
    canvas = np.full((out_h, out_w, 3), WHITE, dtype=np.uint8)
    canvas[(out_h - nh) // 2:(out_h - nh) // 2 + nh,
           (out_w - nw) // 2:(out_w - nw) // 2 + nw] = small
    return canvas


def render_blue(text, W=320, H=96):
    """蓝底白字的整牌样式(更接近真实照片, 但模型认得没上面那种好)"""
    font = cv2.FONT_HERSHEY_SIMPLEX
    img = np.full((H, W, 3), BLUE, dtype=np.uint8)
    t = max(2, H // 24)
    cv2.rectangle(img, (t, t), (W - t - 1, H - t - 1), WHITE, t)

    head, tail = (text[0], text[1:]) if len(text) > 1 else (text, "")
    thick = max(2, int(H / 26))
    dot = max(2, H // 22)

    margin = W // 12
    avail = W - 2 * margin
    scale = H / 48.0 * 0.85
    for _ in range(40):
        (w1, h1), _ = cv2.getTextSize(head, font, scale, thick)
        (w2, h2), _ = cv2.getTextSize(tail, font, scale, thick)
        if w1 + 2 * dot + w2 + (W // 24) * 2 <= avail or scale < 0.2:
            break
        scale *= 0.92

    (w1, h1), _ = cv2.getTextSize(head, font, scale, thick)
    (w2, h2), _ = cv2.getTextSize(tail, font, scale, thick)
    gap = W // 24
    total = w1 + 2 * dot + w2 + gap * 2
    x = (W - total) // 2
    base_y = (H + h1) // 2

    cv2.putText(img, head, (x, base_y), font, scale, WHITE, thick, cv2.LINE_AA)
    x += w1 + gap
    cv2.circle(img, (x + dot, H // 2), dot, WHITE, -1)
    x += 2 * dot + gap
    cv2.putText(img, tail, (x, base_y), font, scale, WHITE, thick, cv2.LINE_AA)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default="test_plate.jpg")
    ap.add_argument("-t", "--text", default="B12345")
    ap.add_argument("--style", choices=["chars", "blue"], default="chars",
                    help="chars=白底黑字(模型认得准), blue=蓝底白字整牌")
    ap.add_argument("-w", "--width", type=int, default=400)
    ap.add_argument("-H", "--height", type=int, default=120)
    ap.add_argument("-j", "--jpeg-quality", type=int, default=95)
    a = ap.parse_args()

    if a.style == "blue":
        img = render_blue(a.text, a.width, a.height)
    else:
        img = render_chars(a.text, a.width, a.height)

    cv2.imwrite(a.out, img, [int(cv2.IMWRITE_JPEG_QUALITY), a.jpeg_quality])
    print(f"已生成 {a.out}  ({img.shape[1]}x{img.shape[0]})  style={a.style}  字样: {a.text}")


if __name__ == "__main__":
    main()
