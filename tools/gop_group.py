#!/usr/bin/env python3
"""gop_group.py —— 把 Annex-B 裸流按 GOP 归并成"访问单元", 并可逐 GOP 改写 SPS

为什么要归并(step/05 引爆测试):
    相机通过 RTP 逐包发送, SPS / PPS / IDR 分散在不同的 RTP 包里。
    FFmpeg 的 h264 demuxer 靠 h264 parser 找访问单元边界, 而这个 parser
    在本相机的码流上是坏的(报 "sps_id 0 out of range"), 于是 SPS/PPS/IDR
    不会被归并到一起 —— MPP 拿到的 IDR 前面从来没有 SPS, 于是永远解不出帧
    (实时 RTSP 正常, 因为 RTSP demuxer 用 RTP 时间戳/标记位定边界, 不靠 parser)。

    本脚本把每个 GOP(SPS+PPS+IDR+若干 slice)重新打包成一个连续段,
    顺便把 SPS 提到每个 GOP 的最前面。这样:
      ① MPP 能正常解码(验证手段可用)
      ② 每个 GOP 的 SPS 都可以独立改写 -> 分辨率来回跳 -> 逼 MPP info-change
         重建 buffer group -> dma-buf fd 被关闭/复用 -> 撞 P0-1

用法:
    python3 gop_group.py <in.h264> <out.h264> [--flip-w 1280] [--flip-h 144]
                         [--flip-every N]   # 每 N 个 GOP 切一次分辨率

不带 --flip-* 时只做归并(用于验证 MPP 能否解码)。
"""

import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from sps_flip import split_nals, patch_sps, find_sps_fields, ue_bits  # noqa: E402

NAL_NAMES = {1: "非IDR", 5: "IDR", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD", 12: "FILLER"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile")
    ap.add_argument("outfile")
    ap.add_argument("--flip-w", type=int, default=0, help="目标宽度(像素), 0=不改")
    ap.add_argument("--flip-h", type=int, default=0, help="目标高度(像素), 0=不改")
    ap.add_argument("--flip-every", type=int, default=1, help="每 N 个 GOP 切一次")
    args = ap.parse_args()

    data = open(args.infile, "rb").read()
    nals = split_nals(data)
    print(f"[gop_group] 输入 {len(data)} 字节, {len(nals)} 个 NAL")

    # 1) 先看原始 SPS 的分辨率与 ue 长度, 确定改写是否可行
    sps_list = [(k, nal) for k, (_, nal) in enumerate(nals) if (nal[0] & 0x1F) == 7]
    if not sps_list:
        print("[gop_group] 找不到 SPS", file=sys.stderr)
        return 1
    _, first_sps = sps_list[0]
    w_off, h_off, w_val, h_val, profile = find_sps_fields(first_sps[1:])
    print(f"[gop_group] 原始 SPS: profile={profile} {w_val * 16}x{h_val * 2} "
          f"(ue {ue_bits(w_val)}/{ue_bits(h_val)} 比特)")

    do_flip = args.flip_w > 0 and args.flip_h > 0
    if do_flip:
        nw = args.flip_w // 16
        nh = args.flip_h // 2
        try:
            patch_sps(first_sps[1:], nw, nh)
            print(f"[gop_group] 改写目标: {nw * 16}x{nh * 2}")
        except ValueError as e:
            print(f"[gop_group] 改写不可行: {e}", file=sys.stderr)
            return 2

    # 2) 找 GOP 边界(IDR), 把每个 GOP 的 NAL 归并成一段
    gops = []
    cur = []
    for k, (_, nal) in enumerate(nals):
        t = nal[0] & 0x1F
        cur.append((k, nal))
        if t == 5:      # IDR 结束一个 GOP 的头部, 但 slice 还在后面
            pass
    # 以 IDR 为界: 每个 IDR 开启一个新 GOP
    gops = []
    cur = []
    for k, (_, nal) in enumerate(nals):
        t = nal[0] & 0x1F
        if t == 5 and cur:
            gops.append(cur)
            cur = []
        cur.append((k, nal))
    if cur:
        gops.append(cur)
    print(f"[gop_group] 切成 {len(gops)} 个 GOP")

    # 3) 每个 GOP: SPS/PPS 提到最前, 按 flip 规则决定 SPS 用原版还是改写版
    out = bytearray()
    flipped = 0
    for gi, gop in enumerate(gops):
        sps = [(k, n) for k, n in gop if (n[0] & 0x1F) == 7]
        pps = [(k, n) for k, n in gop if (n[0] & 0x1F) == 8]
        other = [(k, n) for k, n in gop if (n[0] & 0x1F) not in (7, 8)]
        use_flip = do_flip and ((gi // max(1, args.flip_every)) % 2 == 1)
        for _, n in sps:
            payload = n[1:]
            if use_flip:
                payload = patch_sps(payload, args.flip_w // 16, args.flip_h // 2)
                flipped += 1
            out += b"\x00\x00\x00\x01" + payload[:1] + payload[1:]
        for _, n in pps:
            out += b"\x00\x00\x00\x01" + n
        for _, n in other:
            out += b"\x00\x00\x00\x01" + n
        # GOP 之间不加分隔, 下一个 GOP 的 SPS 会自然跟随

    open(args.outfile, "wb").write(bytes(out))
    print(f"[gop_group] 输出 {len(out)} 字节 -> {args.outfile}")
    print(f"[gop_group] 改写了 {flipped} 个 SPS (共 {len(gops)} 个 GOP)")
    if do_flip:
        print(f"[gop_group] 分辨率切换 {len(gops) // max(1, args.flip_every)} 次")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
