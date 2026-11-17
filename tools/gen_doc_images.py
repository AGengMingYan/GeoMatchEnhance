#!/usr/bin/env python3
"""Generate documentation images for the GeoGUIDemo manual.

Outputs (into docs/):
  mask_overlay_street.png : template + street-mask exclusion overlay (dark red)
  mask_overlay_nodie.png  : template + nodie-mask exclusion overlay
  model_points_compare.png: no-mask vs street-mask model feature points
"""
import os

import cv2
import numpy as np

TPL = "build_x64/tools/tpl_cross176.png"
STREET = "build_x64/tools/mask_street.png"
NODIE = "build_x64/tools/mask_nodie.png"
OUT = "docs"


def overlay(tpl, mask):
    """same visual as GeoGUIDemo overlayMaskOnTpl: excluded area -> dark purple."""
    vis = cv2.cvtColor(tpl, cv2.COLOR_GRAY2BGR).astype(np.float32)
    m = mask
    if m.size != tpl.shape[:2]:
        m = cv2.resize(m, (tpl.shape[1], tpl.shape[0]), interpolation=cv2.INTER_NEAREST)
    if m.ndim == 3:
        m = cv2.cvtColor(m, cv2.COLOR_BGR2GRAY)
    m = (m > 0).astype(np.uint8)
    for y in range(vis.shape[0]):
        for x in range(vis.shape[1]):
            if m[y, x] == 0:
                b, g, r = vis[y, x]
                vis[y, x] = (b * 0.35 + 45, g * 0.35, r * 0.35 + 45)
    return vis.astype(np.uint8)


def model_points(tpl, mask, zoom=3):
    """approximate the library's model feature points (Sobel ridges) for visualisation."""
    gx = cv2.Sobel(tpl, cv2.CV_32F, 1, 0, 3)
    gy = cv2.Sobel(tpl, cv2.CV_32F, 0, 1, 3)
    mag = cv2.magnitude(gx, gy)
    keep = mag > 40
    if mask is not None:
        m = cv2.resize(mask, (tpl.shape[1], tpl.shape[0]),
                       interpolation=cv2.INTER_NEAREST)
        if m.ndim == 3:
            m = cv2.cvtColor(m, cv2.COLOR_BGR2GRAY)
        keep &= (m > 0)
    vis = cv2.cvtColor(tpl, cv2.COLOR_GRAY2BGR)
    for y in range(tpl.shape[0]):
        for x in range(tpl.shape[1]):
            if keep[y, x]:
                cv2.circle(vis, (x, y), 1, (0, 0, 255), -1)
    return cv2.resize(vis, None, fx=zoom, fy=zoom, interpolation=cv2.INTER_NEAREST)


def main():
    tpl = cv2.imread(TPL, cv2.IMREAD_GRAYSCALE)
    street = cv2.imread(STREET, cv2.IMREAD_GRAYSCALE)
    nodie = cv2.imread(NODIE, cv2.IMREAD_GRAYSCALE)

    cv2.imwrite(os.path.join(OUT, "mask_overlay_street.png"), overlay(tpl, street))
    cv2.imwrite(os.path.join(OUT, "mask_overlay_nodie.png"), overlay(tpl, nodie))

    no_mask = model_points(tpl, None)
    street_pts = model_points(tpl, street)
    nodie_pts = model_points(tpl, nodie)
    h = max(no_mask.shape[0], street_pts.shape[0], nodie_pts.shape[0])
    labels = ["no mask (1200 pts)", "street mask (397 pts)", "nodie mask (1200 pts)"]
    tiles = []
    for img, lb in ((no_mask, labels[0]), (street_pts, labels[1]), (nodie_pts, labels[2])):
        pad = h - img.shape[0]
        if pad > 0:
            img = cv2.copyMakeBorder(img, 0, pad, 0, 0, cv2.BORDER_CONSTANT, value=(20, 20, 20))
        tiles.append(img)
    canvas = np.hstack(tiles)
    y = 20
    x = 10
    for lb in labels:
        cv2.putText(canvas, lb, (x, y), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 1, cv2.LINE_AA)
        x += no_mask.shape[1]
    cv2.imwrite(os.path.join(OUT, "model_points_compare.png"), canvas)

    print("generated docs/mask_overlay_street.png, mask_overlay_nodie.png, model_points_compare.png")


if __name__ == "__main__":
    main()
