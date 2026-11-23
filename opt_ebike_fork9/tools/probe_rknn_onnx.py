#!/usr/bin/env python3
"""Probe: does the production .rknn embed a full ONNX ModelProto we can recover?

The RKNN container (rknn-toolkit2 v2.3.2) stores the *original* ONNX graph
(next to the compiled NPU graph) so the model can be re-exported / inspected.
If we can pull that proto out, we get the ebike source graph back and can
rebuild a fork9 (9-output, DFL-on-CPU) variant from the *same weights*.
"""
import sys, os, re

RKNN = '/home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn'
data = open(RKNN, 'rb').read()
print('file size:', len(data))

# where are the ONNX node names / initializer names?
for pat in (b'onnx::Split_388', b'model.22.dfl.conv.weight', b'model.22.cv3.0.0.conv.weight',
            b'"ori_network_platform"', b'images'):
    i = data.find(pat)
    print('  %-32s @ %s' % (pat.decode()[:32], i))

# The RKNN header: magic(4) version(4) ... size field at 0x10
import struct
print('header words:', [hex(x) for x in struct.unpack('<8I', data[:32])])

# Scan for an ONNX ModelProto: ir_version field 1 varint == 7 or 8, then graph tag 0x3A
cands = []
for o in range(len(data) - 4):
    if data[o] == 0x08 and data[o + 1] in (6, 7, 8) and data[o + 2] == 0x3A:
        cands.append(o)
print('candidate ModelProto offsets:', len(cands), cands[:20])
