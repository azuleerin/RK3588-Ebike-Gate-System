#!/usr/bin/env python3
"""ts_flip.py —— 把访问单元列表封成 MPEG-TS, 并可逐 GOP 改写 SPS 分辨率

为什么自己写 TS 封装(step/05 引爆测试):
    FFmpeg 的 mpegts muxer 会(1)给每个关键帧插入 AUD(NAL 9),
    (2)把超过 64KB 的访问单元拆成多个 PES。
    而本相机的 IDR 访问单元本身就是 113KB, 必然要拆。
    自己封装可以精确控制: 不插 AUD、PES 里原样放访问单元。
    FFmpeg 的 mpegts demuxer 会原样吐 PES 负载, 于是 MPP 拿到的
    访问单元结构与实时 RTSP 一致(SEI+SPS+PPS+IDR), 能正常解码。

用法:
    python3 ts_flip.py <in.aus> <out.ts> [--flip-w 1280] [--flip-h 144]
                        [--flip-every N]

    in.aus 是 es_dump --aus 产出的"4字节长度 + 访问单元"序列。
"""

import argparse
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from sps_flip import split_nals, patch_sps, find_sps_fields, ue_bits  # noqa: E402

TS_PKT = 188
PID_PAT = 0x0000
PID_PMT = 0x1000
PID_VID = 0x0100

# ---- MPEG-2 section 的 CRC32 (poly 0x04C11DB7), 与 zlib.crc32 不同 ----
_CRC_TABLE = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if (_c & 0x80000000) else (_c << 1) & 0xFFFFFFFF
    _CRC_TABLE.append(_c)


def mpeg_crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[((crc >> 24) ^ b) & 0xFF]
    return crc


def ts_header(pid, pusi, cc, payload_start, payload):
    """组一个 TS 包"""
    b = bytearray(TS_PKT)
    b[0] = 0x47
    b[1] = (0x40 if pusi else 0x00) | ((pid >> 8) & 0x1F)
    b[2] = (pid & 0xFF)
    # adaptation_field_control: 0x30 = 有 adaptation 且含 PCR; 0x10 = 仅 adaptation
    af_len = 7 if payload_start else 0
    if af_len:
        b[3] = 0x30 | (cc & 0x0F)
        b[4] = af_len - 1
        b[5] = 0x50            # PCR_flag
        pcr = payload_start & 0x01FFFFFFFFFFFFFF
        b[6] = (pcr >> 25) & 0xFF
        b[7] = (pcr >> 17) & 0xFF
        b[8] = (pcr >> 9) & 0xFF
        b[9] = (pcr >> 1) & 0xFF
        b[10] = 0x01 | ((pcr & 1) << 7)   # reserved + PCR[0]
        b[11] = 0x00
        off = 12
    else:
        b[3] = 0x10 | (cc & 0x0F)
        b[4] = 0x00            # adaptation_field_length = 0
        off = 5
    space = TS_PKT - off - len(payload)
    if space < 0:
        raise ValueError("payload 太大")
    if space:
        b[off - 1] = space - 1   # 修正 adaptation_field_length
        # stuffing bytes 填 0xFF
        for i in range(off, off + space):
            b[i] = 0xFF
        off += space
    b[off:off + len(payload)] = payload
    return bytes(b)


def make_pat():
    section = bytearray()
    section += bytes([0x00])            # table_id = PAT
    section += bytes([0xB0, 0x0D])      # section_syntax_indicator=1, length=13
    section += struct.pack(">H", 1)      # transport_stream_id
    section += bytes([0xC1, 0x00, 0x00])  # version=0, current, section 0/0
    section += struct.pack(">H", 1)      # program_number
    section += bytes([0xE0 | ((PID_PMT >> 8) & 0x1F), PID_PMT & 0xFF])
    crc = mpeg_crc32(bytes(section))
    section += struct.pack(">I", crc)
    return bytes([0x00]) + bytes(section)   # pointer_field=0 + section


def make_pmt():
    section = bytearray()
    section += bytes([0x02])            # table_id = PMT
    section += bytes([0xB0, 0x12])      # length = 18
    section += struct.pack(">H", 1)      # program_number
    section += bytes([0xC1, 0x00, 0x00])
    section += struct.pack(">H", PID_VID)  # PCR_PID
    section += bytes([0xF0, 0x00])        # program_info_length
    section += bytes([0x1B])              # stream_type = H.264
    section += bytes([0xE0 | ((PID_VID >> 8) & 0x1F), PID_VID & 0xFF])
    section += bytes([0xF0, 0x00])        # ES_info_length
    crc = mpeg_crc32(bytes(section))
    section += struct.pack(">I", crc)
    return bytes([0x00]) + bytes(section)


def encode_pts(pts90k):
    """33bit PTS/DTS -> 5 字节"""
    b = bytearray(5)
    b[0] = ((pts90k >> 30) & 0x07) << 4 | 0x20 | ((pts90k >> 28) & 0x0F)
    b[1] = ((pts90k >> 22) & 0xFF)
    b[2] = ((pts90k >> 14) & 0xFC) | 0x04 | ((pts90k >> 13) & 0x03)
    b[3] = ((pts90k >> 6) & 0xFC) | 0x04 | ((pts90k >> 4) & 0x03)
    b[4] = ((pts90k << 1) & 0xFE) | 0x01
    return bytes(b)


def write_pes(f, cc, payload, pts, dts=None, pusi=True):
    """写一个 PES(可能超过 64KB -> 拆成多个 PES, video 流允许 length=0)"""
    if dts is None:
        dts = pts
    hdr = bytearray()
    hdr += bytes([0x00, 0x00, 0x01, 0xE0])   # start code + stream_id
    hdr += bytes([0x00, 0x00])                 # PES_packet_length = 0 (video 不限)
    hdr += bytes([0x80, 0x80, 0x05])           # '10' + flags + header_data_length
    hdr += encode_pts(pts)
    hdr += encode_pts(dts)
    blob = bytes(hdr) + payload
    # PES 负载要跨多个 TS 包: 带 PCR 适应域时每包最多 176 字节负载,
    # 只有第一个带 PUSI(=1 表示 PES 起点)。
    off = 0
    first = True
    while off < len(blob):
        chunk = blob[off:off + 170]
        off += len(chunk)
        f.write(ts_header(PID_VID, pusi and first, cc, pts if first else 0, chunk))
        cc = (cc + 1) & 0x0F
        first = False
    return cc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile")
    ap.add_argument("outfile")
    ap.add_argument("--flip-w", type=int, default=0)
    ap.add_argument("--flip-h", type=int, default=0)
    ap.add_argument("--flip-every", type=int, default=1)
    ap.add_argument("--fps", type=int, default=25)
    args = ap.parse_args()

    # 读访问单元列表
    data = open(args.infile, "rb").read()
    aus = []
    off = 0
    while off + 4 <= len(data):
        ln = struct.unpack_from("<I", data, off)[0]
        off += 4
        if off + ln > len(data):
            break
        aus.append(data[off:off + ln])
        off += ln
    print(f"[ts_flip] 读入 {len(aus)} 个访问单元, {len(data)} 字节")
    if not aus:
        print("[ts_flip] 没有访问单元", file=sys.stderr)
        return 1

    # 找出带 SPS 的 AU, 确认改写可行
    sps_aus = [i for i, au in enumerate(aus)
               if any((nal[0] & 0x1F) == 7 for _, nal in split_nals(au))]
    print(f"[ts_flip] {len(sps_aus)} 个 AU 带 SPS")
    if sps_aus:
        nal = [n for _, n in split_nals(aus[sps_aus[0]]) if (n[0] & 0x1F) == 7][0]
        w_off, h_off, w_val, h_val, prof = find_sps_fields(nal[1:])
        print(f"[ts_flip] 原分辨率 mbs {w_val}x{h_val} (ue {ue_bits(w_val)}/{ue_bits(h_val)} 比特)")

    do_flip = args.flip_w > 0 and args.flip_h > 0
    if do_flip:
        try:
            patch_sps(nal[1:], args.flip_w // 16, args.flip_h // 2)
            print(f"[ts_flip] 改写目标 {args.flip_w}x{args.flip_h}")
        except ValueError as e:
            print(f"[ts_flip] {e}", file=sys.stderr)
            return 2

    # 写 TS
    cc_pat, cc_pmt, cc_vid = 0, 0, 0
    pts = 0
    step = 90000 // args.fps
    flipped = 0
    gop = 0
    with open(args.outfile, "wb") as f:
        for _ in range(4):     # PAT/PMT 多发几遍, 兼容 demuxer 启动时机
            f.write(ts_header(PID_PAT, True, cc_pat, 0, make_pat()))
            cc_pat = (cc_pat + 1) & 0x0F
            f.write(ts_header(PID_PMT, True, cc_pmt, 0, make_pmt()))
            cc_pmt = (cc_pmt + 1) & 0x0F
        for au in aus:
            # 需要改写 SPS 的 AU: 判断该 AU 是否带 SPS, 以及按 GOP 计数决定用哪个版本
            has_sps = any((nal[0] & 0x1F) == 7 for _, nal in split_nals(au))
            payload = au
            if do_flip and has_sps:
                use_flip = ((gop // max(1, args.flip_every)) % 2 == 1)
                gop += 1
                if use_flip:
                    out = bytearray()
                    for _, nal in split_nals(au):
                        if (nal[0] & 0x1F) == 7:
                            p = patch_sps(nal[1:], args.flip_w // 16, args.flip_h // 2)
                            out += nal[:1] + p
                            flipped += 1
                        else:
                            out += nal
                    payload = bytes(out)
            cc_vid = write_pes(f, cc_vid, payload, pts, pts)
            pts = (pts + step) & 0x1FFFFFFFF

    print(f"[ts_flip] 输出 TS -> {args.outfile}")
    print(f"[ts_flip] 改写了 {flipped} 个 SPS, 共 {gop} 个 GOP")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
