# Halcon 17 形状匹配对比 Demo

用仓库内自带的 Halcon 17（32 位 SDK，位于 `HalconDemo/Halcon17/`）跑
`create_scaled_shape_model` / `find_scaled_shape_model`，输出格式与本项目
`GeoMatchDemo` 对齐，用于逐项评估两者在**分数 / 位置 / 角度 / 尺度 / 速度**上的实际差距。

## 依赖与架构

| 项 | 说明 |
|---|---|
| Halcon SDK | `HalconDemo/Halcon17/`（32 位：`x86sse2-win32/halconcpp.lib` + `bin/*.dll` + `include/`） |
| 编译器 | MSVC **x86**（`vcvars32.bat`），不能与主项目 x64 构建混链 |
| 运行时 | `PATH` 需含 `HalconDemo/Halcon17/bin`，`HALCONARCH=x86sse2-win32` |

> Halcon 17 本机只有 32 位库，所以本 Demo 是独立的 Win32 程序；与主项目 x64 的
> `GeoMatchDemo` **分别运行**，再比对两者输出。

## 构建

```bat
cmd /c tools\build-halcon-compare.bat
```

产物：`HalconDemo/halcon_shape_compare.exe`（32 位，不入库，`.gitignore` 已忽略）。

脚本自动完成：加载 `vcvars32.bat` → 设 `HALCONARCH=x86sse2-win32` → 把 Halcon
`bin` 加进 `PATH` → `cl.exe` 编译链接 `halconcpp.lib`。

> 若你的 VS 不在 `D:\develop2\vs2019`，改脚本里的 `VSVARS` 变量。

## 运行

```bat
set PATH=HalconDemo\Halcon17\bin;%PATH%
set HALCONARCH=x86sse2-win32
set HALCONROOT=HalconDemo\Halcon17

HalconDemo\halcon_shape_compare.exe <template> [search_image] [options]
```

示例（与本项目 `GeoMatchDemo -t2 -load` 同参数对比）：

```bat
HalconDemo\halcon_shape_compare.exe build\case2\template_a.jpg build\case2\search_a_03.jpg -minscore 0.3 -max 1
```

输出：

```
model created  tpl=...  create=16.41 ms
angle: -180.0..180.0 deg  scale: 1.000..1.000  minscore=0.300  maxmatches=1  overlap=1.00
subpixel=true  levels=0  greediness=0.90  contrast=30  mincontrast=12

...search_a_03.jpg  matches=1  search=6.94 ms
   #0  score=0.9350  pos=(639.21;512.28)  angle=44.86deg  scale=1.0000
```

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `-angle-start <deg>` | -180 | 起始角度 |
| `-angle-extent <deg>` | 360 | 角度范围 |
| `-smin <scale>` | 1.0 | 最小尺度 |
| `-smax <scale>` | 1.0 | 最大尺度 |
| `-minscore <score>` | 0.7 | 最小得分 |
| `-max <N>` | 12 | 最多匹配数 |
| `-max-overlap <v>` | 1.0 | 最大重叠率（Halcon 默认 0.5，这里默认 1.0 对齐本项目） |
| `-subpixel <true\|false>` | true | 亚像素精化（Halcon 的 `SubPixel`） |
| `-levels <N>` | 0 | 金字塔层数，0 = 自动 |
| `-greediness <v>` | 0.9 | 贪婪因子 |
| `-contrast <v>` | 30 | 高阈值 |
| `-min-contrast <v>` | 12 | 低阈值 |
| `-save-model <path>` | — | 保存模型（`write_shape_model`） |
| `-load-model <path>` | — | 加载模型（跳过创建） |
| `-loop <N>` | 1 | 重复查找 N 次取最快耗时 |

## 与本项目的对应关系

| 本项目（GeoMatch/ShapeMatch） | Halcon | 说明 |
|---|---|---|
| `MatchParams::contrast` | `Contrast`（高阈值） | 建模型参数 |
| `MatchParams::minContrast` | `MinContrast`（低阈值） | 建模型参数 |
| `FindParams::minScore` | `MinScore` | 评分阈值 |
| `FindParams::metric=0` | `Metric='use_polarity'` | 梯度极性 |
| `FindParams::subpixel` | `SubPixel` | 亚像素精化 |
| `FindParams::maxOverlap` | `MaxOverlap` | 重叠抑制 |
| `FindParams::greediness` | `Greediness` | 搜索贪婪因子 |
| `MatchResult::score`（scoreMode=0 mean） | `Score` | 两者语义一致 |

> 本项目默认 `scoreMode=0`（mean），语义与 Halcon 的 `Score` 对齐；
> 对比时两边用同一 `minScore`、同一角度/尺度范围、同一模板与搜索图。

## 注意事项

1. **32 位**：本 Demo 是 Win32 程序，不能与主项目 x64 目标放进同一个构建目录。
2. **密文图片**：若图片在本机被加密（文件头非 `FF D8 FF` / `89 50 4E 47`），
   Halcon 直接读会 abort；先用 `cat.exe` 重定向解密回原名再跑（见 `.clinerules`）。
3. **License**：本机实测无需额外 `.dat` 也能正常 `create/find`（未报 #2106），
   若其它机器报 license 错误，需把合法 license 放到 `HALCONROOT/license/`。
4. **计时口径**：`search=` 是 `find_scaled_shape_model` 单次调用耗时（不含 `imread`），
   与 `GeoMatchDemo` 的 `search=`（不含 imread/绘制）同口径，可直接对比。
