#!/usr/bin/env python3
"""Locate the bright-green match box / cyan ROI box / label rows in a screenshot."""
import sys

import cv2
import numpy as np

path = sys.argv[1]
what = sys.argv[2] if len(sys.argv) > 2 else 'green'
im = cv2.imread(path)
h, w = im.shape[:2]
print('%s %dx%d mode=%s' % (path, w, h, what))

b = im[:, :, 0].astype(np.int32)
g = im[:, :, 1].astype(np.int32)
r = im[:, :, 2].astype(np.int32)

if what == 'green':
    mask = (g > 180) & (r < 120) & (b < 120)
elif what == 'cyan':
    mask = (g > 150) & (b > 150) & (r < 140)
else:
    raise SystemExit('mode green|cyan')

ys, xs = np.where(mask)
if xs.size == 0:
    print('nothing found')
    raise SystemExit(0)
print('bbox x=%d..%d  y=%d..%d  count=%d' % (xs.min(), xs.max(), ys.min(), ys.max(), xs.size))

# strongest rows / cols (frame lines of the box)
rows = mask.sum(1)
cols = mask.sum(0)
top = np.argsort(rows)[-8:][::-1]
topc = np.argsort(cols)[-8:][::-1]
print('busiest rows:', sorted(int(v) for v in top))
print('busiest cols:', sorted(int(v) for v in topc))
