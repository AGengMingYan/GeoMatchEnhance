# -*- coding: utf-8 -*-
"""生成「多角度 / 多缩放」合成样本，用于验证形状匹配的角度与尺度精度。

做法与 GeoMatch::runGenTest 一致：
    1. 生成一张带线条/圆圈的杂乱背景（避免匹配器靠背景蒙对）；
    2. 用 warpAffine 把模板按 (角度, 缩放) 放到画面正中；
    3. 叠加高斯噪声，模拟真实相机；
    4. 文件名直接编码真值：synth_a+45.0_s085.jpg 表示角度 +45.0°、缩放 0.85。

用法：
    python tools/gen_synth.py --template case1/temp.jpg --out dataset/sample/synth

注意（本仓库开发机）：本机文件加密驱动会按「写文件的进程」加密，python 写出的
图片在磁盘上是密文（首字节 E0 A8 91 ...）。入库/上传前必须用 Git 自带的
cat.exe 解密回原名：
    cat.exe a.jpg > out.jpg
详见 .clinerules 第 1/2 节。其它机器上没有这个问题。
"""
import argparse
import math
import os

import cv2
import numpy as np


def parse_list(text):
    return [float(v) for v in text.replace(';', ',').split(',') if v.strip()]


def main():
    ap = argparse.ArgumentParser(description='generate rotated/scaled synthetic samples')
    ap.add_argument('--template', default='case1/temp.jpg', help='模板图路径')
    ap.add_argument('--out', default='dataset/sample/synth', help='输出目录')
    ap.add_argument('--size', type=int, default=700, help='输出图像边长（正方形）')
    ap.add_argument('--quality', type=int, default=92, help='JPEG 质量（0=存 PNG）')
    ap.add_argument('--angles', default='0,15,-15,30,-30,45,-45,60,-60,90,-90,120,-120,135,-135,180',
                    help='角度列表（度，逗号分隔）')
    ap.add_argument('--scales', default='0.70,0.85,1.00,1.15,1.30',
                    help='缩放列表（逗号分隔）')
    ap.add_argument('--noise', type=float, default=6.0, help='高斯噪声标准差（灰度）')
    ap.add_argument('--seed', type=int, default=12345, help='随机种子（保证可复现）')
    args = ap.parse_args()

    tpl = cv2.imread(args.template, cv2.IMREAD_GRAYSCALE)
    if tpl is None:
        raise SystemExit('cannot read template: %s' % args.template)
    print('template %s  %dx%d' % (args.template, tpl.shape[1], tpl.shape[0]))

    angles = parse_list(args.angles)
    scales = parse_list(args.scales)
    os.makedirs(args.out, exist_ok=True)

    side = int(args.size)
    cx = cy = side / 2.0
    tcx, tcy = tpl.shape[1] / 2.0, tpl.shape[0] / 2.0
    rng = np.random.default_rng(args.seed)

    total = len(angles) * len(scales)
    done = 0
    for a_deg in angles:
        for scale in scales:
            a = math.radians(a_deg)
            ca, sa = math.cos(a), math.sin(a)

            # 旋转 + 缩放后，让模板中心落在画面中心
            tx = cx - scale * (ca * tcx + sa * tcy)
            ty = cy - scale * (-sa * tcx + ca * tcy)
            M = np.array([[scale * ca, scale * sa, tx],
                          [-scale * sa, scale * ca, ty]], dtype=np.float64)

            # 杂乱背景
            big = np.full((side, side), 40, dtype=np.uint8)
            for _ in range(8):
                p1 = (int(rng.integers(0, side)), int(rng.integers(0, side)))
                p2 = (int(rng.integers(0, side)), int(rng.integers(0, side)))
                cv2.line(big, p1, p2, int(rng.integers(50, 200)), 1)
            for _ in range(4):
                c = (int(rng.integers(0, side)), int(rng.integers(0, side)))
                r = int(rng.integers(20, 60))
                cv2.circle(big, c, r, int(rng.integers(60, 200)), 2)

            warped = cv2.warpAffine(tpl, M, (side, side), flags=cv2.INTER_LINEAR,
                                    borderMode=cv2.BORDER_CONSTANT, borderValue=40)
            m8u = np.full(tpl.shape, 255, dtype=np.uint8)
            wmask = cv2.warpAffine(m8u, M, (side, side), flags=cv2.INTER_NEAREST,
                                   borderMode=cv2.BORDER_CONSTANT, borderValue=0)
            big[wmask > 0] = warped[wmask > 0]

            noise = np.random.normal(0, args.noise, (side, side)).astype(np.float32)
            big = np.clip(big.astype(np.float32) + noise, 0, 255).astype(np.uint8)

            ext = 'png' if args.quality <= 0 else 'jpg'
            name = 'synth_a%+05.1f_s%03d.%s' % (a_deg, int(round(scale * 100)), ext)
            path = os.path.join(args.out, name)
            if ext == 'png':
                cv2.imwrite(path, big)
            else:
                cv2.imwrite(path, big, [cv2.IMWRITE_JPEG_QUALITY, args.quality])

            done += 1
            if done % 10 == 0 or done == total:
                print('  %d/%d  %s' % (done, total, name))

    print('done: %d files -> %s' % (done, args.out))


if __name__ == '__main__':
    main()
