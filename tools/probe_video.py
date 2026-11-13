#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_video.py —— 视频体检(不需要 ffmpeg/ffprobe)

为什么要它:
    板子上【没有装 ffmpeg/ffprobe】, 现场拿到一段新视频时,
    "这条码流 VPU 能不能解" 这个判断本来只能靠 ffmpeg 问。
    这个脚本直接读 MP4 容器本身, 把 VPU 关心的那几项一次问清楚:
        编码(H.264/H.265)、profile、位深(8/10 bit)、分辨率、帧率、帧数、
        参数集是 Annex-B 还是 AVCC 封装。

用法:
    python3 tools/probe_video.py <视频文件.mp4>

输出示例:
    文件      : 电动车素材.mp4  (142.3 MB)
    编码      : H.265 / HEVC (Main 10)      ← ★ 10bit, VPU 可能不收
    位深      : 10 bit  (luma)
    分辨率    : 1920x1080
    帧率      : 25.00 fps  (timebase 1/1000)
    帧数      : 10500
    封装      : AVCC (MP4 长度前缀)   → 板端会自动转成 Annex-B 再喂 MPP
    VPU 预判  : ⚠️ 10bit —— 建议先转成 8bit(H.264/yuv420p)
"""
from __future__ import annotations

import os
import struct
import sys


# ---------------------------------------------------------------------------
# MP4 盒子解析(只取需要的几个, 不做完整索引)
# ---------------------------------------------------------------------------
def iter_boxes(data, start, end):
    """在 [start, end) 区间里逐个吐出 (type, payload_start, payload_end)"""
    off = start
    while off + 8 <= end:
        size = struct.unpack(">I", data[off:off + 4])[0]
        btype = data[off + 4:off + 8].decode("latin1")
        hdr = 8
        if size == 1:                      # 64 位长度
            size = struct.unpack(">Q", data[off + 8:off + 16])[0]
            hdr = 16
        elif size == 0:                    # 到文件末尾
            size = end - off
        if size < hdr:
            break
        yield btype, off + hdr, off + size
        off += size


def find_box(data, path, start=0, end=None):
    """按路径找盒子, 例: find_box(data, ['moov','trak','mdia','minf','stbl','stsd'])"""
    if end is None:
        end = len(data)
    cur_start, cur_end = start, end
    for want in path:
        hit = None
        for btype, s, e in iter_boxes(data, cur_start, cur_end):
            if btype == want:
                hit = (s, e)
                break
        if hit is None:
            return None
        cur_start, cur_end = hit
    return (cur_start, cur_end)


def read_stsd_entries(data, s, e):
    """stsd 盒: 前 8 字节是版本/条目数, 后面是若干 sample entry"""
    n = struct.unpack(">I", data[s + 4:s + 8])[0]
    entries = []
    off = s + 8
    for _ in range(n):
        if off + 8 > e:
            break
        size = struct.unpack(">I", data[off:off + 4])[0]
        fmt = data[off + 4:off + 8].decode("latin1")
        entries.append((fmt, off, off + size))
        off += size
    return entries


# ---------------------------------------------------------------------------
# SPS 解析: 目的是拿到【位深】
# ---------------------------------------------------------------------------
def rbsp(nal):
    """去掉 emulation prevention byte (0x00 0x00 0x03 -> 0x00 0x00)"""
    out = bytearray()
    i = 0
    while i < len(nal):
        if i + 2 < len(nal) and nal[i] == 0 and nal[i + 1] == 0 and nal[i + 2] == 3:
            out += nal[i:i + 2]
            i += 3
        else:
            out.append(nal[i])
            i += 1
    return bytes(out)


class Bits:
    def __init__(self, data):
        self.d = data
        self.pos = 0

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.d[self.pos >> 3]
            bit = (byte >> (7 - (self.pos & 7))) & 1
            v = (v << 1) | bit
            self.pos += 1
        return v

    def ue(self):
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
            if zeros > 32:
                return 0
        return (1 << zeros) - 1 + (self.u(zeros) if zeros else 0)


HIGH_PROFILES = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135}


def h264_sps_bitdepth(sps):
    """H.264: profile_idc 在高规格集里, SPS 里带 bit_depth_luma_minus8"""
    b = Bits(rbsp(sps))
    profile_idc = b.u(8)
    b.u(8)          # constraint flags
    b.u(8)          # level_idc
    b.ue()          # seq_parameter_set_id
    if profile_idc not in HIGH_PROFILES:
        return 8, profile_idc
    chroma = b.ue()
    if chroma == 3:
        b.u(1)
    luma = b.ue() + 8
    b.ue()          # bit_depth_chroma_minus8
    return luma, profile_idc


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    if not os.path.exists(path):
        print(f"❌ 文件不存在: {path}")
        return 1

    size = os.path.getsize(path)
    with open(path, "rb") as f:
        data = f.read()          # 视频文件不大, 直接读进内存最省事

    print(f"文件      : {os.path.basename(path)}  ({size / 1024 / 1024:.1f} MB)")

    # ---- 找到视频 sample entry ----
    stsd = find_box(data, ["moov", "trak", "mdia", "minf", "stbl", "stsd"])
    if not stsd:
        print("❌ 这不是 MP4/MOV 文件(找不到 moov/trak/stsd)")
        return 1
    entries = read_stsd_entries(data, stsd[0], stsd[1])
    if not entries:
        print("❌ 没找到视频轨")
        return 1
    fmt, es, ee = entries[0]

    # ---- 分辨率: 在 sample entry 里(偏移 24/26 是宽/高, 16.16 定点) ----
    # VisualSampleEntry 布局: 盒子头(8) + reserved(6) + data_ref_index(2)
    #   + pre_defined(2) + reserved(2) + pre_defined[3](12) + width(2) + height(2)
    #   -> width 在样本条目起始(off, 含盒子头)之后第 24 字节, 即 off+32
    width = height = 0
    if ee - es > 36:
        width = struct.unpack(">H", data[es + 32:es + 34])[0]
        height = struct.unpack(">H", data[es + 34:es + 36])[0]

    codec = "?"
    bit_depth = 8
    profile = "?"
    container = "?"

    # ---- 编码类型: sample entry 的 format 标签直接说明了 ----
    if fmt in ("avc1", "avc3"):
        codec = "H.264 / AVC"
    elif fmt in ("hvc1", "hev1"):
        codec = "H.265 / HEVC"

    # ---- 参数集盒子(avcC/hvcC)在 sample entry 的【扩展区】, 起始于 +86 字节 ----
    # VisualSampleEntry 固定字段共 78 字节, 加上 8 字节盒子头 -> 子盒子从 +86 开始
    for btype, s, e in iter_boxes(data, es + 86, ee):
        if btype == "avcC":
            profile_idc = data[s + 1]
            # 第 5 个字节的 bits 0-1: lengthSizeMinusOne
            length_size = (data[s + 4] & 0x03) + 1
            sps_len = struct.unpack(">H", data[s + 6:s + 8])[0]
            sps = data[s + 8:s + 8 + sps_len]
            bd, prof = h264_sps_bitdepth(sps)
            codec = "H.264 / AVC"
            bit_depth = bd
            profile = f"profile_idc={prof}"
            # 参数集是不是 Annex-B: SPS 开头应为 00 00 00 01
            if sps[:4] == b"\x00\x00\x00\x01":
                container = "Annex-B"
            else:
                container = f"AVCC (长度前缀 {length_size} 字节)"
            break
        if btype == "hvcC":
            profile_idc = data[s + 1] & 0x1F
            bit_depth = (data[s + 18] & 0x07) + 8
            codec = "H.265 / HEVC"
            profile = f"profile_idc={profile_idc}"
            container = "AVCC (MP4 长度前缀)"
            break

    # ---- 帧率与帧数: stts ----
    fps = 0.0
    nframes = 0
    stts = find_box(data, ["moov", "trak", "mdia", "minf", "stbl", "stts"])
    mdhd = find_box(data, ["moov", "trak", "mdia", "mdhd"])
    timescale = 0
    if mdhd:
        ver = data[mdhd[0]]
        if ver == 1:
            timescale = struct.unpack(">I", data[mdhd[0] + 20:mdhd[0] + 24])[0]
        else:
            timescale = struct.unpack(">I", data[mdhd[0] + 12:mdhd[0] + 16])[0]
    if stts:
        s = stts[0]
        n = struct.unpack(">I", data[s + 4:s + 8])[0]
        total_dur = 0
        for i in range(n):
            cnt, delta = struct.unpack(">II", data[s + 8 + i * 8:s + 16 + i * 8])
            nframes += cnt
            total_dur += cnt * delta
        if timescale and total_dur:
            fps = nframes * timescale / total_dur

    print(f"编码      : {codec}  ({profile})")
    print(f"位深      : {bit_depth} bit" + ("  ← ★ 10bit, 板端 VPU 大概率解不了" if bit_depth > 8 else ""))
    print(f"分辨率    : {width}x{height}")
    print(f"帧率      : {fps:.2f} fps" + (f"  (timebase 1/{timescale})" if timescale else ""))
    print(f"帧数      : {nframes}")
    print(f"封装      : {container}")

    # ---- VPU 预判 ----
    # RK3588 VPU 实测边界(2026-11-13 在本板上验证过):
    #   * H.264 8bit  ✅ 正常
    #   * H.265 8bit  ✅ 正常
    #   * H.265 10bit ✅ 也能解(内置 DJI 航拍 MP4 就是 HEVC Main10, 实测出帧正常)
    #   * H.264 10bit ⚠️ 高规格(High10)不在 VPU 支持范围内, 建议转成 8bit
    print()
    if codec.startswith("H.264") and bit_depth > 8:
        print("VPU 预判  : ⚠️ H.264 高位深(10bit)不在 RK3588 VPU 支持范围, 建议先转成 8bit:")
        print("            ffmpeg -i 电动车素材.mp4 -c:v libx264 -profile:v high -pix_fmt yuv420p "
              "-r 25 -an 电动车素材_h264_8bit.mp4")
    elif codec.startswith("H.264"):
        print("VPU 预判  : ✅ H.264 8bit —— 板端 MPP 直接可解")
        print('            跑起来后确认日志有 "[解码] 后端 = Rockchip MPP 硬解" 和 "[MPP] 硬解出帧"')
    elif codec.startswith("H.265"):
        print(f"VPU 预判  : ✅ H.265 {bit_depth}bit —— 板端 MPP 可解(10bit 已实测过)")
        print('            跑起来后确认日志有 "[解码] 后端 = Rockchip MPP 硬解" 和 "[MPP] 硬解出帧"')
    else:
        print(f"VPU 预判  : ❌ 编码是 {codec}, 不在 MPP 支持范围内(H.264/H.265 才行)")
        print("            必须先转码:  ffmpeg -i 电动车素材.mp4 -c:v libx264 -pix_fmt yuv420p -r 25 "
              "-an 电动车素材_h264.mp4")
    return 0


if __name__ == "__main__":
    sys.exit(main())
