# image：Halcon 经典示例图（形状匹配多实例测试）

这是一组无配套模板的灰度图（PNG/BMP，单通道），用来验证形状匹配在
**多实例、任意旋转、多尺度**场景下的定位与去重能力。图片本身高对比、
目标是黑色实心件，适合直接用默认对比度建模。

## 文件构成

| 文件 | 尺寸 | 内容 | 用途 |
|---|---|---|---|
| `m1.bmp` | 207×185 | 单个卡簧（E 形挡圈） | 卡簧模板 |
| `m2.bmp` | 207×197 | 单个回收箭头 logo | logo 模板 |
| `m3.bmp` | 102×101 | 单个齿轮 | 齿轮模板 |
| `m4.bmp` | 105×92 | 单个六角螺母 | 螺母模板 |
| `q1.bmp` | 347×262 | 双环 logo | logo 场景 |
| `nuts_01..03.png` | 640×480 | 13 个不同尺寸/朝向的螺母 | 多尺度多实例 |
| `retainers_01..03.png` | 640×480 | 多个齿轮 | 同尺度多实例（含旋转） |
| `rings_01..06.png` | 640×480 | 卡簧 + 齿轮混放 | 卡簧多实例 / 干扰物 |
| `green-dot.png` / `green-dots.png` | 640×482 | 回收标识实拍 | logo 实拍 |
| `qfn1.bmp` / `qfn2.bmp` | 2448×2048 | QFN 芯片大图 | 大图性能 |

## 怎么跑

一键批量（按「单目标模板 → 多实例场景」分组，结果写 `_ai_tmp/imtest/`，
汇总在 `summary.csv`）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\test-image-dataset.ps1
```

单条命令示例（齿轮模板 → 多齿轮场景，360° 旋转，去重）：

```bat
build\GeoMatchDemo.exe -t dataset\image\m3.bmp -s dataset\image\retainers_02.png ^
    -angle 360 -minscore 0.5 -max 30 -max-overlap 0.5 -min-dist 40 ^
    -noshow -o _ai_tmp\imtest\gear
```

多尺度（螺母有大有小，默认 `k=1` 只检出同尺寸的 3 个，开尺度后全检出）：

```bat
build\GeoMatchDemo.exe -t dataset\image\m4.bmp -s dataset\image\nuts_01.png ^
    -angle 360 -smin 0.4 -smax 1.7 -minscore 0.5 -max 30 ^
    -max-overlap 0.5 -min-dist 25 -noshow -o _ai_tmp\imtest\nut_scale
```

## 实测结果（x64 Release，2026-11）

固定尺度（`-angle 360 -max-overlap 0.5 -min-dist 40`）：

| 模板 | 场景 | 检出 | 最高分 | 说明 |
|---|---|---|---|---|
| 齿轮 m3 | retainers_01/02/03 | 各 5 | 0.91~1.00 | 同尺度齿轮全检出，轮廓贴合 |
| 螺母 m4 | nuts_01/02/03 | 各 3 | 0.95~0.97 | 只检出与模板同尺寸的大螺母 |
| 卡簧 m1 | rings_02/04 | 3 | 0.999 | 含 1 个低分（0.78）齿轮误检 |
| 卡簧 m1 | rings_01/03/06 | 1~2 | 0.99~1.00 | 卡簧定位准确 |

多尺度（螺母，`-smin 0.4 -smax 1.7`）：

- `nuts_01` 检出 **14 个**（`k=0.64~1.31`），小/中/大螺母全部准确贴合，
  分数 0.91~0.98；代价是单图搜索从约 13 ms 涨到约 825 ms（尺度×角度网格）。

性能（2048×2448 的 `qfn2.bmp`，360° 固定尺度）：预处理 17 ms + 搜索 42 ms ≈ **59 ms/张**。

## 注意事项

- 齿轮/螺母是高旋转对称目标，同一实例会在多个角度产生高分配额，**必须配
  `-max-overlap 0.5`（或 GUI「最大重叠」）去重**，否则一张图会报 20 个重复框。
- 齿轮模板在 `rings_*` 上固定/多尺度都可能 0 命中——这些图里的齿轮与模板
  尺度、外形差异较大，属正常现象，不是缺陷。
- 结果图（带框）由 CLI 写到 `-o` 目录，文件名与搜索图同名。
