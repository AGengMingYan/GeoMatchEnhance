#!/usr/bin/env python3
"""Locate clickable blue button rows in the GeoGUIDemo right panel screenshot.

Prints y-bands (image coords) where a horizontal run of the button blue is found,
plus the x-extent, so automation scripts can click precisely.
"""
import sys

import cv2
import numpy as np

path = sys.argv[1] if len(sys.argv) > 1 else '_ai_tmp/shot_probe.png'
x0 = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
x1 = int(sys.argv[3]) if len(sys.argv) > 3 else 1500

im = cv2.imread(path)
if im is None:
    print('cannot read', path)
    raise SystemExit(1)
h, w = im.shape[:2]
print('image %dx%d, panel x[%d,%d)' % (w, h, x0, x1))

roi = im[:, x0:min(x1, w)].astype(np.int32)
b, g, r = roi[:, :, 0], roi[:, :, 1], roi[:, :, 2]
# ImGui default "frame/button" blue: ~ (66,150,250) BGR -> 0.6*255/.. ; keep it loose
mask = (b - r > 40) & (b > 80) & (r < 130) & (g > 60) & (g < 170)

rows = mask.sum(1)
prev = False
start = 0
for y in range(h):
    cur = rows[y] > (roi.shape[1] * 0.5)
    if cur and not prev:
        start = y
    elif not cur and prev:
        band = mask[start:y]
        cols = np.where(band.any(0))[0]
        if cols.size:
            print('band y=%3d..%3d (h=%2d)  x=%d..%d  centre=(%d,%d)'
                  % (start, y - 1, y - start,
                     x0 + cols[0], x0 + cols[-1],
                     x0 + (cols[0] + cols[-1]) // 2, (start + y) // 2))
    prev = cur
