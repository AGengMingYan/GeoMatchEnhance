#!/usr/bin/env python3
"""Verify the annotated doc screenshots against tools/shot-specs/*.json.

For every spec we know the source crop, the scale and padTop/padBottom, so the
caption rectangles can be reconstructed exactly:

  * the file must be plaintext (PNG / JPG magic bytes);
  * each caption band (padTop / padBottom) must contain dark text pixels
    (grey >=245 background with glyphs of the caption colour);
  * the right-most 4 columns of each caption band must stay near-white,
    i.e. no caption runs off the canvas edge.
"""
import glob
import json
import os

import cv2
import numpy as np


def caption_bands(spec, out_h):
    pad_top = int(spec.get('padTop', 0))
    pad_bot = int(spec.get('padBottom', 0))
    bands = []
    if pad_top:
        bands.append((0, pad_top, 'top'))
    if pad_bot:
        bands.append((out_h - pad_bot, out_h, 'bottom'))
    return bands


def expects_text(spec, y0, y1):
    """True when some absolute-positioned caption falls inside [y0, y1)."""
    for t in spec.get('texts', []):
        if t.get('abs') is True and y0 <= int(t['y']) < y1:
            return True
    return False


fail = 0
specs = sorted(glob.glob('tools/shot-specs/*.json'))
for sp in specs:
    spec = json.load(open(sp, encoding='utf-8'))
    path = spec['out']
    if not os.path.exists(path):
        print('MISSING', path)
        fail += 1
        continue

    raw = open(path, 'rb').read(8)
    ok_magic = (raw[:8] == b'\x89PNG\r\n\x1a\n') or (raw[:3] == b'\xff\xd8\xff')
    im = cv2.imread(path)
    h, w = im.shape[:2]
    gray = cv2.cvtColor(im, cv2.COLOR_BGR2GRAY)
    bands = caption_bands(spec, h)

    notes = []
    for y0, y1, name in bands:
        band = gray[y0:y1]
        dark = int((band < 200).sum())
        clipped = int((band[:, w - 4:] < 235).sum())
        want = expects_text(spec, y0, y1)
        notes.append('%s[%d:%d] ink=%d clip=%d%s'
                     % (name, y0, y1, dark, clipped, '' if want else ' (pad only)'))
        if want and dark < 100:
            notes.append('!! %s caption has no text' % name)
            fail += 1
        if clipped > 40:
            notes.append('!! %s caption clipped at right edge' % name)
            fail += 1

    print('%-24s %4dx%-4d magic=%-3s %s'
          % (os.path.basename(path), w, h, 'ok' if ok_magic else 'BAD',
             '; '.join(notes) or 'no caption'))
    if not ok_magic:
        fail += 1

print('FAILURES =', fail)


