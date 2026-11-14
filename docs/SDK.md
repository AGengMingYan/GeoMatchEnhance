# GeoMatch SDK 使用手册

> 版本 **1.1.0** ｜ Windows x64 / x86 ｜ MSVC 2019+ ｜ OpenCV 4.5.5
>
> 本手册面向**在自己的程序里调用 GeoMatch 库**的场景。只想用现成界面看效果的话，
> 直接运行发布包里的 `GeoGUIDemo.exe` 即可，不需要读这份文档。

---

## 目录

- [1. 这个库能做什么](#1-这个库能做什么)
- [2. 包里有什么](#2-包里有什么)
- [3. 五分钟上手](#3-五分钟上手)
- [4. 链接配置](#4-链接配置)
- [5. 形状匹配 API](#5-形状匹配-api)
- [6. NCC 匹配 API](#6-ncc-匹配-api)
- [7. 参数调优](#7-参数调优)
- [8. 线程安全与性能](#8-线程安全与性能)
- [9. 常见问题](#9-常见问题)
- [10. 完整示例](#10-完整示例)

---

## 1. 这个库能做什么

两套**互相独立**的模板匹配算法，接口风格对标 Halcon：

| | 形状匹配（`geomatch::Matcher`） | NCC（`geomatch::NccMatcher`） |
|---|---|---|
| 比什么 | 边缘的**梯度方向** | **归一化灰度互相关** |
| 对应 Halcon | `create_shape_model` / `find_shape_model` | `create_ncc_model` / `find_ncc_model` |
| 旋转 | 支持，任意角度 | 支持，任意角度 |
| 尺度 | 支持（`scaleMin`/`scaleMax`） | **不支持**（Halcon 的 NCC 也不支持） |
| 低对比度 / 模糊 / 纯纹理 | 容易点数塌陷 | **更稳** |
| 重复纹理 | **更稳**（能区分相似区域） | 处处命中，不可用 |
| 典型耗时（1280x1024，154x148 模板，360 度） | 约 46 ms | 约 20 ms |

**怎么选**：先试形状匹配；如果在低对比度/纹理表面上不稳定，再换 NCC。

---

## 2. 包里有什么

```
GeoMatch-1.1.0-win64/
├── include/
│   ├── GeoMatch/GeoMatch.h        # 门面（平时只 include 这个）
│   ├── ShapeMatch/ShapeMatch.h    # 形状匹配底层引擎
│   └── NccMatch/NccMatch.h        # NCC 引擎
├── lib/
│   ├── GeoMatch.lib               # 库（默认静态）
│   └── opencv_world455.lib        # OpenCV 4.5.5 导入库
├── bin/
│   ├── GeoMatch.dll               # 只有 shared 版才有
│   └── opencv_world455.dll        # 运行时必须有
├── lib/cmake/GeoMatch/            # CMake 包配置（find_package 用）
└── SDK.md                         # 本文件
```

> x86 与 x64 是**两套独立**的包（OpenCV 二进制不通用），不能混用。

---

## 3. 五分钟上手

### 3.1 最小程序（形状匹配）

```cpp
#include "GeoMatch/GeoMatch.h"

int main()
{
    cv::Mat tpl = cv::imread("temp.jpg", cv::IMREAD_GRAYSCALE);
    cv::Mat img = cv::imread("scene.jpg", cv::IMREAD_GRAYSCALE);

    geomatch::Config cfg;               // 建模 + 搜索参数都在这里
    cfg.minScore = 0.7;                 // 默认评分是 mean；偏保守(误检优先)，追召回可调低

    geomatch::Matcher m;
    if (!m.create(tpl, cfg)) return 1;  // 或 m.load("model.shm")

    geomatch::Timing tm;
    auto dets = m.detect(img, &tm);
    for (auto& d : dets)
        printf("score %.3f  pos (%.1f, %.1f)  angle %.1f\n",
               d.score, d.pos.x, d.pos.y, d.angle * 180.0 / CV_PI);

    cv::imwrite("out.png", m.drawResult(img, dets));
    return 0;
}
```

编译（MSVC 命令行，静态库）：

```bat
cl /std:c++17 /EHsc /MD /O2 /utf-8 ^
   /I <sdk>\include ^
   demo.cpp ^
   <sdk>\lib\GeoMatch.lib <sdk>\lib\opencv_world455.lib
```

运行时把 `<sdk>\bin\opencv_world455.dll` 放到 exe 旁边。

### 3.2 模型能存盘复用

```cpp
m.create(tpl, cfg);
m.save("model.shm");       // 建一次，之后直接 load，省建模时间
// ...
geomatch::Matcher m2;
m2.load("model.shm");       // 注意：load 会把 cfg 恢复成模型里存的建模参数，
m2.setConfig(cfg);          // 搜索参数（minScore/maxMatches…）用 setConfig 覆盖
```

---

## 4. 链接配置

### 4.1 方式 A：CMake（推荐）

```cmake
cmake_minimum_required(VERSION 3.10)
project(myapp CXX)

find_package(GeoMatch REQUIRED)          # 把 <sdk>/lib/cmake/GeoMatch 加到 GeoMatch_DIR
find_package(OpenCV REQUIRED)            # 或者手动指定 opencv_world455.lib

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE GeoMatch::GeoMatch ${OpenCV_LIBS})
```

不想装到系统目录、直接给路径也行：

```cmake
set(GeoMatch_DIR "C:/sdk/GeoMatch-1.1.0-win64/lib/cmake/GeoMatch")
find_package(GeoMatch REQUIRED)
```

### 4.2 方式 B：手工指定（任何构建系统）

| 项 | 值 |
|---|---|
| 头文件 | `-I <sdk>\include` |
| 库 | `<sdk>\lib\GeoMatch.lib` |
| OpenCV | `<sdk>\lib\opencv_world455.lib` |
| 运行时 | `<sdk>\bin\opencv_world455.dll`（DLL 版还要 `GeoMatch.dll`） |
| 预处理器 | 静态库：`GEOMATCH_STATIC`（可省略）；DLL：**必须** `GEOMATCH_SHARED` |

> **重要**：头文件里靠 `GEOMATCH_SHARED` 决定 `__declspec(dllimport)`。
> 用 DLL 版却没定义这个宏，链接时会报 `unresolved external symbol`。

### 4.3 静态库 vs DLL

| | 静态 `GeoMatch.lib` | 动态 `GeoMatch.dll` |
|---|---|---|
| 部署 | 只带自己的 exe | 还要带 `GeoMatch.dll` |
| C++ 运行时 | 必须与库一致：`/MD`（Release） | 同左 |
| 编译宏 | `GEOMATCH_STATIC` | `GEOMATCH_SHARED` |

发布包同时提供两者（`lib/` 与 `bin/`）。

### 4.4 必须的编译选项

- **C++17**（`/std:c++17`）
- **`/MD`**（Release 用多线程 DLL 运行时）。库就是用 `/MD` 编的，混用 `/MT` 会报 `LNK2038`
- 源码含中文注释时加 `/utf-8`（否则 MSVC 按 cp936 读会告警/乱码）
- 想拿到 SIMD 性能：Release 加 `/O2 /arch:AVX2`（发布包就是这么编的）。
  **目标 CPU 必须支持 AVX2**；不支持就自己用 `/arch:SSE2` 重编

---

## 5. 形状匹配 API

头文件：`GeoMatch/GeoMatch.h`

### 5.1 `geomatch::Config`

一个结构体同时承载**建模**和**搜索**参数（Halcon 里分成两个算子，这里合到一起）。

**建模参数**

| 字段 | 默认 | 含义 |
|---|---|---|
| `angleStart` | `-CV_PI` | 起始角（弧度） |
| `angleExtent` | `2*CV_PI` | 角度范围（弧度） |
| `angleStep` | `0` | 角度步长；`0` = 自动 |
| `scaleMin` / `scaleMax` | `1.0` | 尺度范围；不需要就都填 1.0（最快） |
| `contrast` | `30.0` | 滞后**高**阈值（灰度差），种子边 |
| `minContrast` | `12.0` | 滞后**低**阈值（灰度差），弱边能否保留 |
| `maxPoints` | `1200` | 最多保留多少个模型点 |
| `numLevels` | `0` | 金字塔层数；`0` = 自动。**不要填 1** |
| `mask` | 空 | `CV_8U`，非零 = 参与建模 |
| `minChainLen` | `2` | 丢弃短于这个长度（像素）的孤立边缘链 |

**搜索参数**

| 字段 | 默认 | 含义 |
|---|---|---|
| `minScore` | `0.7` | 最低分数 |
| `maxMatches` | `12` | 最多返回几个实例 |
| `minDistance` | `30.0` | 两实例中心的最小像素距离 |
| `maxOverlap` | `1.0` | Halcon 的 `MaxOverlap`；1.0 = 关闭，0.5 是 Halcon 默认 |
| `greediness` | `0.9` | 搜索贪心度，越大越快越可能漏 |
| `subpixel` | `1` | `0` 关 / `1` 抛物线（默认）/ `2` 最小二乘 |
| `metric` | `0` | `0` 要求极性 / `1` 忽略整体反转 / `2` 忽略逐点极性 |
| `scoreMode` | `0` | `0` mean（**与 Halcon 一致**）/ `2` trimmed / `1` positive-mean（**会虚高**）/ `3` clipped |
| `minCoverage` | `0` | 最小覆盖率，`0` = 关闭 |
| `skipWeight` | `0.9` | 评分时跳过图像梯度幅值低于 `minContrast` 的模型点（**对齐 Halcon 语义**），分母用有效点数；`0` = 关闭。修复前模糊/弱对比帧分数会系统性低 ~0.3 |
| `cellMissMax` | `0.35` | 与 `skipWeight` 配套：把模板分 4×4 格，若"图像上几乎没梯度的格"占比超过此值则拒绝该匹配。**防止"只对上模板一部分"的错位实例被 skipWeight 抬到 0.9+**；`0` = 关闭 |
| `useOpenCL` | `true` | 是否尝试 GPU 预处理 |
| `debug` | `0` | >0 打印调试信息 |

> **两项评分稳健性改进已内建（无开关，默认生效）：**
> 1. **软门限梯度归一化**：弱梯度按 `min(mag/minContrast,1)` 给部分权重而非直接归零，
>    轻微模糊/光照变化下分数不再塌到 0.4~0.7 区间；只改分数刻度，不改变哪些像素算边缘。
> 2. **亚像素梯度采样**：精化/复评在模型点真实旋转位置（亚像素）双线性插值采样图像梯度，
>    消除整数取整的 ≤0.5px 抖动，斜角角度误差实测降约 1/3；粗扫仍用整数卷积核保速度。
>    注意亚像素插值会让分数刻度整体略下移，真实小角度场景影响很小（实测 7 张实拍图
>    默认 minScore=0.7 全命中），但 45° 以上的极端旋转重采样可能需要适当调低 `minScore`。

> **`scoreMode` 千万不要用 1**。它的定义是 `clipped / coverage`，
> 只在"方向一致"的点上取平均；错位置上仍有九成点方向一致，于是报 0.95+。
> 实测同一位姿：`scoreMode=1` 得 0.946，`scoreMode=0` 得 0.567，Halcon 0.556。

### 5.2 `geomatch::Detection`

| 字段 | 类型 | 含义 |
|---|---|---|
| `pos` | `cv::Point2f` | 实例中心（搜索图像坐标，亚像素） |
| `angle` | `float` | 旋转角（**弧度**） |
| `scale` | `float` | 尺度倍率 |
| `score` | `float` | 分数（`scoreMode=0` 时与 Halcon 同量级） |
| `coverage` | `float` | 覆盖率（`scoreMode!=0` 或 `minCoverage>0` 时才有值） |

---


### 5.3 `geomatch::Matcher`

| 方法 | 说明 |
|---|---|
| `bool create(const cv::Mat& gray, const Config&)` | 建模型。失败返回 false（通常是阈值太高、点太少） |
| `bool load(const std::string& file)` | 读 `.shm` |
| `bool save(const std::string& file) const` | 写 `.shm` |
| `bool ready() const` | 有没有可用模型 |
| `std::vector<Detection> detect(const cv::Mat& gray, Timing* = nullptr) const` | 搜索 |
| `detectTimed(gray, loops, Timing*)` | 重复 `loops` 次取最后一次耗时（做基准测试用） |
| `cv::Mat drawResult(gray, dets) const` | 结果叠加图（BGR） |
| `cv::Mat drawModelFeatures(scale=3.0, showGradients=false) const` | 模型可视化 |
| `cv::Mat drawModelInfo(title="") const` | 模型图 + 参数面板 |
| `BatchReport runBatch(files, outDir, writeImages, loops, onImage)` | 批量跑目录 |
| `int modelPointCount() const` | 模型点数（<150 要警惕） |
| `int numLevels() const` | 实际金字塔层数 |
| `cv::Size modelSize()` / `modelCenter()` / `modelGray()` | 模型几何 |
| `Config config() const` / `void setConfig(const Config&)` | 读/改搜索参数 |
| `int candidatePixels() const` | 过阈值的候选像素数（判断阈值是否真在起作用） |
| `float pointSpreadRatio() const` | 点散布比；>2.5 说明点几乎排成一条线 |
| `float normalPeakShare() const` | 法线最集中方向占比；>0.6 位置判别力弱 |
| `float strongestMag()` / `weakestMag()` | 保留点里的最强/最弱梯度幅值 |
| `ShapeMatcher& core()` | 逃生口，拿底层引擎 |

### 5.4 `geomatch::Timing`

| 字段 | 含义 |
|---|---|
| `preprocessMs` | 预处理（Sobel 等）耗时 |
| `searchMs` | 搜索耗时 |
| `readMs` / `drawMs` | 只有 `runBatch()` 会填 |
| `recognitionMs()` | `preprocessMs + searchMs` |

### 5.5 自由函数

| 函数 | 说明 |
|---|---|
| `const char* versionString()` | `"1.1.0"` |
| `bool isSharedBuild()` | 当前链接的是 DLL 还是静态库 |
| `std::vector<std::string> listImages(dir, exts)` | 列目录里的图片（已排序） |
| `bool enableOpenCL(bool)` | 开关 OpenCL，返回实际是否可用 |
| `std::string environmentReport()` | OpenCV 版本 / CPU 特性 / 设备 |
| `std::string benchmarkReport()` | 机器基准（1/2/4/全部线程） |
| `std::vector<std::string> excludeFile(files, tplPath)` | 把模板本身从列表里去掉 |

---

## 6. NCC 匹配 API

头文件：`NccMatch/NccMatch.h`

### 6.1 `geomatch::NccParams`（建模）

| 字段 | 默认 | 含义 |
|---|---|---|
| `angleStart` | `-CV_PI` | 起始角 |
| `angleExtent` | `2*CV_PI` | 角度范围 |
| `angleStep` | `0.0` | 0 = 自动（1 度） |
| `numLevels` | `0` | 0 = 自动 |
| `metric` | `0` | `0` use_polarity / `1` ignore_global_polarity |
| `mask` | 空 | `CV_8U`，非零 = 参与相关 |

### 6.2 `geomatch::NccFindParams`（搜索）

| 字段 | 默认 | 含义 |
|---|---|---|
| `minScore` | `0.7` | 分数范围 `[-1,1]`，0.7 左右算"像"，0.8 以上基本确定 |
| `maxMatches` | `1` | 最多返回几个 |
| `maxOverlap` | `0.5` | 实例重叠超过这个比例只留高分那个 |
| `subpixel` | `1` | `0` 关 / `1` 抛物线 |
| `numLevels` | `0` | 从哪一层开始搜，0 = 用模型的层数 |
| `debug` | `0` | 1 = 打印分段耗时，2 = 再打印候选表 |

### 6.3 `geomatch::NccResult`

| 字段 | 含义 |
|---|---|
| `pos` | 中心坐标 |
| `angle` | 角度（弧度） |
| `score` | `[-1,1]`；`metric=1` 时是绝对值 |

### 6.4 `geomatch::NccMatcher`

| 方法 | 说明 |
|---|---|
| `bool createNccModel(gray, NccParams)` | 建模 |
| `bool writeNccModel(file)` / `readNccModel(file)` | `.ncm` 存/读 |
| `std::vector<uchar> serializeNccModel()` / `deserializeNccModel(data)` | 内存序列化 |
| `std::vector<NccResult> findNccModel(gray, NccFindParams) const` | 搜索 |
| `double probe(gray, pos, angle) const` | 给某一位姿打分（不做搜索） |
| `drawResult(gray, results)` / `drawModelFeatures(scale)` | 可视化 |
| `lastPreprocessTime()` / `lastSearchTime()` | 最近一次耗时 |
| `modelSize()` / `modelPixelCount()` / `numLevels()` / `angleCount()` / `angleStep()` | 模型信息 |

---


## 7. 参数调优

### 7.1 顺序（按有效性）

1. **`scoreMode` 保持 0**（mean）。想要 Halcon 可比分数就只能用这个。
2. **别把 `minContrast` 推过头**。以点数为准：`modelPointCount()` 掉到 150 以下就回退。
3. **模板要有二维结构**：转角、字符、缺口。纯直线段/纯边缘带救不了（孔径问题）。
4. **确认模板真在场景里**：用 NCC 自检一次，正常应 ≥0.8；只有 0.2 说明"找不到"才是正确答案。
5. **收窄 ROI 或涂掩码**，只留唯一结构。
6. `minCoverage` 与 `scoreMode=3` 只在**点够多**时有用。

### 7.2 建完模型先自检

```cpp
printf("points %d  spread %.2f  normalPeak %.2f\n",
       m.modelPointCount(), m.pointSpreadRatio(), m.normalPeakShare());
```

| 指标 | 健康 | 危险 | 含义 |
|---|---|---|---|
| 点数 | >= 300 | < 150 | 分数的证据量 |
| 点散布比 | <= 2.0 | > 2.5 | 1 = 团块，越大越接近一条线 |
| 法线最集中占比 | <= 0.6 | > 0.6 | 单一走向主导 -> 位置判别力弱 |

### 7.3 速度

| 做法 | 效果 |
|---|---|
| `numLevels = 0`（自动，不要填 1） | 1 层比 3 层慢 130 倍且结果相同 |
| `scaleMin == scaleMax` | 去掉整个尺度维度 |
| 收窄 `angleExtent` | 角度数是线性成本 |
| 缩小模板 / `maxPoints` | 点越少越快 |
| `greediness` 提到 0.9~0.95 | 更快，但可能漏检弱目标 |

---

## 8. 线程安全与性能

- **建模不是线程安全的**：`create()` / `load()` / `createNccModel()` 会改对象状态，
  同一个对象不要多线程同时建模。
- **搜索是线程安全的**：`detect()` / `findNccModel()` 都是 `const`，
  一个建好的模型可以被多线程同时搜。
- **多相机 / 多工位**：每个线程一个 `Matcher`，各自 `load()` 同一个 `.shm` 即可
  （模型文件只读）。
- **内部并行**：库内部用 `cv::parallel_for_` 吃满所有核。宿主程序自己也开线程池时，
  可以用 `cv::setNumThreads(N)` 限制，避免过度订阅。
- **OpenCL**：默认开启，但很多机器只有集显、收益为负。测过再决定
  `cfg.useOpenCL = false` 或 `geomatch::enableOpenCL(false)`。

`benchmarkReport()` 能一次看清机器上 Sobel / 解包的吞吐随线程数的变化：

```cpp
printf("%s\n", geomatch::benchmarkReport().c_str());
```

---

## 9. 常见问题

**Q: 链接报 `unresolved external symbol "class geomatch::Matcher::Matcher"`**
用 DLL 版时没定义 `GEOMATCH_SHARED`；或者 32/64 位混用了包。

**Q: 链接报 `LNK2038: mismatch detected for 'RuntimeLibrary'` 或 `_ITERATOR_DEBUG_LEVEL`**
两种常见原因：
1. 工程用了 `/MT`，而库是 `/MD` 编的 → 改成 `/MD`；
2. **工程是 Debug 配置，而 SDK 只提供 Release 库** → 用 Release 配置编你的程序，
   或者自己用 `-DCMAKE_BUILD_TYPE=Debug` 编一份 Debug 库。

**Q: 能只要 Debug 版库吗？**
发布包里只带 Release（体积小、也是实际部署用的）。要 Debug 就从源码
`cmake -DCMAKE_BUILD_TYPE=Debug` 自己编一份。

**Q: `create()` 返回 false**
`minContrast`/`contrast` 太高导致点数不足。降阈值或提高 `maxPoints`；
也可以用 `candidatePixels()` 判断到底是阈值在卡还是 `maxPoints` 在卡。

**Q: 分数很高但位置是错的**
先确认 `scoreMode == 0`。其次是点数塌陷 / 孔径问题，看 7.2 的三个指标。

**Q: 明明有目标却 0 命中**
- 模板真的在场景里吗（NCC 自检 >= 0.8）
- `minScore` 是不是太高（`mean` 模式下真匹配常在 0.75~0.99）
- 角度范围够不够（模板可能旋转超出了 `angleExtent`）
- 尺度范围够不够

**Q: 32 位程序能用吗？**
能，发布包提供 win32 与 win64 两套。注意两套 OpenCV 二进制不通用，不能混链。

**Q: 需要 OpenCV 的哪些模块？**
发布包用的是 `opencv_world455`（单一大库）。自己编至少要 `core imgproc imgcodecs`。

---

## 10. 完整示例

仓库 `examples/` 下有两份**可编译**的完整程序（构建时会一起编，保证文档不过期）：

| 文件 | 内容 |
|---|---|
| `examples/example_shape_match.cpp` | 形状匹配：读图 -> 建模 -> 搜索 -> 打印 -> 存标注图 |
| `examples/example_ncc_match.cpp` | NCC：读图 -> 建模 -> 搜索 -> 打印 -> 存标注图 |

运行：

```bat
build_x64\example_shape_match.exe case1\temp.jpg case1\tmp_grab_1762994178.jpg out.png
build_x64\example_ncc_match.exe   case1\temp.jpg case1\tmp_grab_1762994178.jpg out.png
```

输出示例：

```
GeoMatch 1.1.0 (static build)
template 154x148   image 1280x1024
model: 1200 points, 3 levels, 154x148, spread 1.14
matches: 1   preprocess 2.36 ms   search 46.12 ms
  #0  score 0.981  pos (841.97, 674.43)  angle -0.1 deg  scale 1.000
```

更多进阶用法（批量、基准、自检、可视化）见仓库 `main.cpp`（`GeoMatchDemo`）
与 `GeoGUIDemo.cpp`（ImGui 界面），两者都只用公开头文件。

