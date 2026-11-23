#!/usr/bin/env python3
"""Probe 2: locate an embedded ONNX ModelProto region inside the .rknn container.

Strategy: the production model's graph output names must appear inside the
ONNX proto (graph.output ValueInfoProto) *and* inside the RKNN metadata JSON.
Find every occurrence; if some occurrences are far from the end (the JSON),
we have found the ONNX region.
"""
import re, sys
F = '/home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn'
data = open(F, 'rb').read()
print('size', len(data))

for needle in (b'/model.22/Mul_2_output_0', b'/model.22/Sigmoid_output_0',
               b'onnx::Split_388', b'model.22.dfl.conv.weight',
               b'onnx::', b'Conv', b'dfl'):
    offs = [m.start() for m in re.finditer(re.escape(needle), data)]
    print('%-32s n=%-5d %s' % (needle.decode(), len(offs), offs[:12]))

# A ModelProto starts with field 1 (ir_version) varint.
# Walk backwards from the first "onnx::" node name and try each 0x08-prefixed
# candidate, but bound the parse buffer to the region end to keep it cheap.
first_node = data.find(b'onnx::')
print('first onnx:: node-name at', first_node)
