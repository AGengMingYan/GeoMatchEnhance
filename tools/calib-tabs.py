#!/usr/bin/env python3
"""Print x-bands of the ImGui tab bar (selected tab is brighter)."""
import sys

import cv2
import numpy as np

path = sys.argv[1]
y0 = int(sys.argv[2]) if len(sys.argv) > 2 else 55
y1 = int(sys.argv[3]) if len(sys.argv) > 3 else 80
x0 = int(sys.argv[4]) if len(sys.argv) > 4 else 180
x1 = int(sys.argv[5]) if len(sys.argv) > 5 else 700

im = cv2.imread(path)
roi = im[y0:y1, x0:x1].astype(np.int32)
b, g, r = roi[:, :, 0], roi[:, :, 1], roi[:, :, 2]
mask = (b - r > 40) & (b > 80) & (r < 130)
cols = mask.sum(0)
prev = False
start = 0
for i, v in enumerate(cols):
    cur = v > (y1 - y0) * 0.5
    if cur and not prev:
        start = i
    elif not cur and prev:
        print('tab x=%4d..%4d (w=%3d) centre=%d' % (x0 + start, x0 + i - 1, i - start, x0 + (start + i) // 2))
    prev = cur
