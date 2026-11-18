# GeoMatch — 形状匹配 / NCC 模板匹配库（C++17 + OpenCV 4.x）

工业视觉用的**模板匹配**库：给一张模板图，在一张大图里找出它的**位置、角度、缩放**
（亚像素精度），支持多实例。接口与语义**对齐 Halcon** 的
`create_shape_model` / `find_shape_model` 与 `create_ncc_model` / `find_ncc_model`，
Halcon 配方可以基本一对一搬过来。

- **形状匹配（推荐默认）**：边缘梯度方向相关，抗光照、抗部分遮挡，**带尺度维度**
- **NCC 匹配**：灰度相关，重复纹理 / 低对比 / 模糊表面更稳，**无尺度维度**（与 Halcon 一致）
- 旋转 + 缩放搜索、金字塔由粗到精、模型可存 `.shm`、模板掩码、多实例 NMS
- 附带 `GeoGUIDemo` 图形界面（框选 ROI、涂抹掩码、拖参数看效果）

> 本机实测（i7-13700H，x64 AVX2）：5MP 图 + 360° 形状匹配 **8.3 ms**；
> 1280×1024 + ±10° 焊盘 **4.8 ms**。与 Halcon 17 的分数差 ≤0.016、位置差 ≤0.26 px。

---

## 最简用法（10 行）

```cpp
#include "GeoMatch/GeoMatch.h"                 // 唯一需要 include 的头文件

geomatch::Config cfg;                          // 全部参数都有合理默认值
cfg.angleExtent = 20 * CV_PI / 180;            // 只搜 ±10°（不填 = 360°）
cfg.minScore    = 0.7;

geomatch::Matcher m;
m.create(cv::imread("tpl.png", 0), cfg);       // 也可以 m.load("model.shm")
auto dets = m.detect(cv::imread("scene.png", 0));

for (auto& d : dets)
    printf("score=%.3f pos=(%.1f;%.1f) angle=%.1f scale=%.3f\n",
           d.score, d.pos.x, d.pos.y, d.angle * 180 / CV_PI, d.scale);
```

模型存/取（产线只建一次）：

```cpp
m.save("model.shm");
m.load("model.shm");   // 加载远快于重新建模
```

位姿交给上位机 / 机器人（同一套 3×3 变换导出，彼此自洽）：

```cpp
m.setOrigin(cv::Point2f(20, 40));      // 上报点改成装配基准点（模板像素坐标）
cv::Matx33d M = m.poseMatrix(dets[0]); // 模板 → 图像
auto quad     = m.cornersAt(dets[0]);  // 四角（模板坐标系 TL,TR,BR,BL）
```

NCC 只需换一个类：

```cpp
geomatch::NccMatcher nm;
nm.createNccModel(cv::imread("tpl.png", 0), geomatch::NccParams());
auto res = nm.findNccModel(cv::imread("scene.png", 0), geomatch::NccFindParams());
```

---

## 编译

```bat
tools\build-x64.bat        :: -> build\GeoMatchDemo.exe / NccDemo.exe / GeoGUIDemo.exe
tools\build-x86.bat        :: -> build_x86\...
```

依赖：MSVC 2019+（C++17）、OpenCV 4.5.x。SDK 集成（`find_package(GeoMatch)`）见
[docs/SDK.md](docs/SDK.md)。

命令行最小示例：

```bat
build\GeoMatchDemo.exe -t tpl.png -d img_dir -angle 20 -minscore 0.5 -max 1 -noshow -o out
```

---

## 已对标 Halcon 的函数

| Halcon | 本库 | 状态 |
|---|---|---|
| `create_shape_model` | `Matcher::create` | ✅ 参数全部对齐（含 `Optimization`、多段 `Contrast`） |
| `create_scaled_shape_model` | 同上（`scaleMin/Max` 合进一个结构） | ✅ |
| `find_shape_model` | `Matcher::detect` | ✅ 含 `NumLevels`、搜索期 `AngleStart/AngleExtent` |
| `find_scaled_shape_model` | 同上 | ✅ |
| `write/read_shape_model` | `save()` / `load()`（内存版在核心层 `serializeShapeModel()`） | ✅ |
| `get_shape_model_contours` | `modelChains()` / `contourAt()` | ✅ |
| `get/set_shape_model_origin` | `origin()/setOrigin()/resetOrigin()` | ✅ |
| `set_shape_model_param('timeout')` | `Config::timeoutMs` | ✅ |
| `create_ncc_model` | `NccMatcher::createNccModel` | ✅ 全部参数 |
| `find_ncc_model` | `findNccModel` | ✅ 全部参数（含 `NumLevels`、搜索期角度子范围） |
| `write/read_ncc_model` | `writeNccModel()` / `readNccModel()` | ✅ |
| `determine_shape_model_params` | — | ❌ 缺（自动定参） |
| `find_shape_models`（多模型同搜） | — | ❌ 缺 |
| `create_aniso_shape_model`、`*_xld` | — | ❌ 缺（各向异性 / XLD 建模） |

未对齐项的完整清单、原因与优先级见
[docs/与Halcon差距清单.md](docs/与Halcon差距清单.md)。

---

## 文档

| 想知道什么 | 看哪份 |
|---|---|
| **参数怎么填**（默认值 / 范围 / 含义 / 适用场景） | **[docs/参数手册.md](docs/参数手册.md)** |
| 界面怎么用（每个控件的作用、拖了会发生什么） | [docs/GeoGUIDemo使用说明.md](docs/GeoGUIDemo使用说明.md) |
| 3 分钟跑通 GUI | [docs/快速上手-三步测试Demo.md](docs/快速上手-三步测试Demo.md) |
| SDK 集成、完整 API、线程安全 | [docs/SDK.md](docs/SDK.md) |
| 与 Halcon 的精度/速度实测对比 | [docs/数据集四器综合对比-2026-11-16.md](docs/数据集四器综合对比-2026-11-16.md) |
| 与 Halcon 的差距与优先级 | [docs/与Halcon差距清单.md](docs/与Halcon差距清单.md) |
| 全部文档索引 | [docs/README.md](docs/README.md) |

---

## License

见 [LICENSE](LICENSE)。
