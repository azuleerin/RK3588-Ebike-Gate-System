#!/usr/bin/env python3
"""sps_flip.py —— 就地改写 H.264 SPS 里的分辨率字段(比特长度不变)

用途(step/05 引爆测试):
    要逼 MPP 重建 buffer group(这是 dma-buf fd 被关闭/复用的唯一时机),
    就必须让码流"中途换分辨率"。相机不可控、板上没有 H.264 编码器,
    所以在码流层解决: 把 SPS 里的分辨率字段改掉。

为什么能"就地"改:
    H.264 的 pic_width_in_mbs_minus1 / pic_height_in_map_units_minus1 用
    Exp-Golomb 编码。ue(v) 的比特长度 = 2*floor(log2(v+1))+1, 只取决于数量级。
    只要新值与旧值落在同一个数量级, 编码长度就完全相同 ->
    这两个字段可以原地覆盖, SPS 其余部分(包含 VUI)一个比特都不用动,
    文件长度也不变。

    例: 1080p -> width mbs=119(13bit), height map units=67(13bit)
        目标 1280x144 -> width mbs=79(13bit), height map units=71(13bit)
        两者长度一致 -> 可原地覆盖。

用法:
    python3 sps_flip.py <in.h264> <out.h264> [--new-w 1280] [--new-h 144]
                        [--flip-every N] [--gop-frames N]

    --flip-every N : 每 N 个 GOP 切一次分辨率(默认 1, 即每个 GOP 都切)
    --gop-frames N : 估算每个 GOP 的帧数, 只用于决定切回时机(默认 25)

输出文件的 SPS 会按 GOP 交替使用 原分辨率 / 新分辨率,
让 MPP 反复触发 info-change -> 重建 buffer group。
"""

import argparse
import sys


class BitReader:
    def __init__(self, data):
        self.data = data
        self.pos = 0  # 比特位置

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            bit = (byte >> (7 - (self.pos & 7))) & 1
            v = (v << 1) | bit
            self.pos += 1
        return v

    def ue(self):
        lead = 0
        while self.u(1) == 0:
            lead += 1
            if lead > 32:
                raise ValueError("Exp-Golomb 前导零过多, 不是合法的 SPS")
        if lead == 0:
            return 0
        return (1 << lead) - 1 + self.u(lead)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)

    def skip_bits(self, n):
        self.pos += n

    def skip_ue(self):
        self.ue()

    def skip_se(self):
        self.se()


def ue_bits(v):
    """ue(v) 的比特长度"""
    x = v + 1
    return 2 * (x.bit_length() - 1) + 1


def ue_encode(v, nbits):
    """把 ue(v) 编码成恰好 nbits 比特(调用方保证长度匹配)"""
    lead = nbits // 2
    x = v + 1
    assert x.bit_length() - 1 == lead, \
        f"ue({v}) 需要 {ue_bits(v)} 比特, 与要求的 {nbits} 不符"
    out = []
    for _ in range(lead):
        out.append(0)
    out.append(1)
    for i in range(lead - 1, -1, -1):
        out.append((x >> i) & 1)
    return out


def find_sps_fields(nal_payload):
    """解析 SPS(不含 NAL 头字节), 返回 (width_field_offset, height_field_offset,
    width_val, height_val, profile_idc) —— 偏移是相对 nal_payload 的比特位置"""
    r = BitReader(nal_payload)
    profile_idc = r.u(8)
    r.u(8)          # constraint flags + reserved
    r.u(8)          # level_idc
    r.skip_ue()      # seq_parameter_set_id

    # High profile 系列的额外字段
    if profile_idc in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        chroma_format_idc = r.ue()
        if chroma_format_idc == 3:
            r.skip_bits(1)   # separate_colour_plane_flag
        r.skip_ue()         # bit_depth_luma_minus8
        r.skip_ue()         # bit_depth_chroma_minus8
        r.skip_bits(1)      # qpprime_y_zero_transform_bypass_flag
        seq_scaling_matrix_present_flag = r.u(1)
        if seq_scaling_matrix_present_flag:
            count = 8 if chroma_format_idc != 3 else 12
            for i in range(count):
                if r.u(1):     # seq_scaling_list_present_flag
                    last = 8
                    next_scale = 8
                    for _ in range((16 if i < 6 else 64)):
                        if next_scale != 0:
                            delta = r.se()
                            next_scale = (last + delta + 256) % 256
                        last = next_scale if next_scale != 0 else last

    r.skip_ue()      # log2_max_frame_num_minus4
    poc_type = r.ue()
    if poc_type == 0:
        r.skip_ue()  # log2_max_pic_order_cnt_lsb_minus4
    elif poc_type == 1:
        r.skip_bits(1)
        r.skip_se()
        r.skip_se()
        n = r.ue()
        for _ in range(n):
            r.skip_se()
    r.skip_ue()      # max_num_ref_frames
    r.skip_bits(1)   # gaps_in_frame_num_value_allowed_flag

    w_off = r.pos
    w_val = r.ue()
    h_off = r.pos
    h_val = r.ue()
    return w_off, h_off, w_val, h_val, profile_idc


def patch_sps(nal_payload, new_w_mbs, new_h_map):
    """就地改写分辨率, 返回新 payload(长度必须与原 payload 相同)"""
    w_off, h_off, w_val, h_val, profile = find_sps_fields(nal_payload)

    w_bits = ue_bits(w_val)
    h_bits = ue_bits(h_val)
    if ue_bits(new_w_mbs) != w_bits:
        raise ValueError(
            f"宽度目标不可行: 原 {w_val} 用 {w_bits} 比特, "
            f"目标 {new_w_mbs} 需要 {ue_bits(new_w_mbs)} 比特")
    if ue_bits(new_h_map) != h_bits:
        raise ValueError(
            f"高度目标不可行: 原 {h_val} 用 {h_bits} 比特, "
            f"目标 {new_h_map} 需要 {ue_bits(new_h_map)} 比特")

    # 展开成比特列表再改写
    bits = []
    for byte in nal_payload:
        for i in range(7, -1, -1):
            bits.append((byte >> i) & 1)

    def write_field(offset, code_bits):
        for i, b in enumerate(code_bits):
            bits[offset + i] = b

    write_field(w_off, ue_encode(new_w_mbs, w_bits))
    write_field(h_off, ue_encode(new_h_map, h_bits))

    out = bytearray(len(nal_payload))
    for i, b in enumerate(bits):
        if b:
            out[i >> 3] |= 1 << (7 - (i & 7))
    assert len(out) == len(nal_payload), "改写后长度变了 —— 这不允许"
    return bytes(out)


def split_nals(data):
    """把 Annex-B 切成 [(start_code_len, nal_bytes), ...]"""
    out = []
    i = 0
    n = len(data)
    starts = []
    while i < n - 3:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            starts.append((i, 3))
            i += 3
        elif (data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 0 and data[i + 3] == 1):
            starts.append((i, 4))
            i += 4
        else:
            i += 1
    for k, (pos, sl) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else n
        out.append((sl, data[pos + sl:end]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile")
    ap.add_argument("outfile")
    ap.add_argument("--new-w", type=int, default=1280)
    ap.add_argument("--new-h", type=int, default=144)
    ap.add_argument("--flip-every", type=int, default=1,
                    help="每 N 个 GOP 切一次分辨率")
    ap.add_argument("--gop-frames", type=int, default=25,
                    help="每个 GOP 约多少帧(仅用于打印)")
    args = ap.parse_args()

    data = open(args.infile, "rb").read()
    nals = split_nals(data)
    print(f"[sps_flip] 输入 {len(data)} 字节, {len(nals)} 个 NAL")

    # 先找一个 SPS 看看原分辨率与比特长度
    sps_idx = [k for k, (_, nal) in enumerate(nals) if (nal[0] & 0x1F) == 7]
    if not sps_idx:
        print("[sps_flip] 找不到 SPS(NAL type 7)", file=sys.stderr)
        return 1
    _, nal = nals[sps_idx[0]]
    payload = nal[1:]
    w_off, h_off, w_val, h_val, profile = find_sps_fields(payload)
    print(f"[sps_flip] 原: profile={profile} width_mbs={w_val} height_map={h_val} "
          f"-> {w_val*16}x{h_val*2} (ue 长度 {ue_bits(w_val)}/{ue_bits(h_val)} 比特)")

    new_w_mbs = args.new_w // 16
    new_h_map = args.new_h // 2
    try:
        probe = patch_sps(payload, new_w_mbs, new_h_map)
        _, _, w2, h2, _ = find_sps_fields(probe)
        print(f"[sps_flip] 新: width_mbs={w2} height_map={h2} -> {w2*16}x{h2*2} "
              f"(长度不变: {len(probe)} 字节)")
    except ValueError as e:
        print(f"[sps_flip] {e}", file=sys.stderr)
        print("[sps_flip] 提示: 目标分辨率的 ue 长度必须与原值同量级", file=sys.stderr)
        return 2

    # 按 GOP 交替: 每个 IDR(type 5) 之前若有 SPS, 就按计数决定用原版还是改写版
    out = bytearray()
    gop = 0
    pending_sps = None      # (index_in_nals, payload)
    patched_count = 0
    for k, (sl, nal) in enumerate(nals):
        ntype = nal[0] & 0x1F
        if ntype == 7:
            pending_sps = k
            continue
        if ntype == 5:      # IDR: 新 GOP 开始, 此时决定之前那个 SPS 用哪个版本
            use_patched = ((gop // max(1, args.flip_every)) % 2 == 1)
            gop += 1
            if pending_sps is not None:
                sk, snal = nals[pending_sps]
                if use_patched:
                    newp = patch_sps(snal[1:], new_w_mbs, new_h_map)
                    nals[sk] = (sl, snal[:1] + newp)
                    patched_count += 1
                pending_sps = None

    for sl, nal in nals:
        out += b"\x00" * (sl - 1) + b"\x01" + nal
    open(args.outfile, "wb").write(bytes(out))

    print(f"[sps_flip] 输出 {len(out)} 字节, {gop} 个 GOP, 改写了 {patched_count} 个 SPS")
    print(f"[sps_flip] 每个 GOP 约 {args.gop_frames} 帧 -> "
          f"约 {gop * args.gop_frames / 25.0:.1f} 秒码流, "
          f"分辨率切换 {gop // max(1, args.flip_every)} 次")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
