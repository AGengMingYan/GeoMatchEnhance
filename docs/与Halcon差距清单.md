# 与 Halcon 模板匹配的差距清单（2026-11-16）

> 依据：本仓库 `ShapeMatch/`、`NccMatch/`、`GeoMatch/` 的实际 API 与实现；
> `HalconDemo/Halcon17/include/halconcpp/*.h` 中 Halcon 17 真实存在的算子；
> 以及 `docs/数据集四器综合对比-2026-11-16.md` 的实测数据。
> 每条都标注「证据」与「影响」，方便排优先级。

## 〇、一句话结论

**精度/召回已经不是主要问题**（NCC 同实例 Δscore ≤0.003，Shape 角度误差 0.068° 优于 Halcon 的 0.113°），
真正的差距集中在三块：**① 性能（3~80×）　② 功能覆盖面（多模型 / 搜索 ROI / 各向异性 / 轮廓建模 / 超时）
　③ 工程化（错误码、绑定、CI 回归）**。

---

## 一、能力缺口（Halcon 有，本项目没有）

### 1. 多模型同时搜索 —— 缺

| 项 | 说明 |
|---|---|
| Halcon | `FindShapeModels` / `FindScaledShapeModels` / `FindAnisoShapeModels` / `FindNccModels`：一次调用传模型数组，返回 `Model` 索引，**跨模型统一做 NMS** |
| 本项目 | `Matcher` / `NccMatcher` 都是**单模型单对象**；多模板只能循环调用，跨模型重叠抑制要自己写 |
| 影响 | 多工位/多品种共线检测时，代码量和误检都会增加（重叠框要靠外部再过滤） |

### 2. 各向异性缩放（ScaleR / ScaleC 独立）—— 缺

| 项 | 说明 |
|---|---|
| Halcon | `create_aniso_shape_model(_xld)` + `FindAnisoShapeModel`，行/列缩放分别给范围 |
| 本项目 | 只有等比 `scaleMin/scaleMax`（`Config` 里两个标量） |
| 影响 | 透视/镜头畸变导致的「横竖缩放不同」无法匹配，只能等比重试或放弃 |

### 3. 从轮廓（XLD / CAD）建模 —— 缺

| 项 | 说明 |
|---|---|
| Halcon | `CreateShapeModelXld(HXLDCont ...)`，可直接用 CAD 导出的轮廓建模，不需要实拍灰度图 |
| 本项目 | `create()` 只接受 `cv::Mat` 灰度图（可带 `mask`），必须有一张模板图 |
| 影响 | 「先有图纸、后到产线」的场景无法离线建模；也不能只用某条设计轮廓定位 |

### 4. 搜索区域限制（reduce_domain / ROI）—— 缺

| 项 | 说明 |
|---|---|
| Halcon | 传入 `reduce_domain` 后的图像，只在区域内搜索 |
| 本项目 | `detect(gray)` 总是全图；CLI 无 `-roi`，GUI 的 ROI **只用于裁模板**（`roiOwner` 1/2） |
| 影响 | 大图性能无法靠「缩小搜索区」救；也无法用区域排除已知干扰物 |

### 5. 超时 / 可中断 —— 缺

| 项 | 说明 |
|---|---|
| Halcon | `set_shape_model_param(...,'timeout',...)` + `H_ERR_TIMEOUT`（9400 系列），搜索可限时 |
| 本项目 | `Config` / `FindParams` 没有任何 timeout 字段，也没有取消回调 |
| 影响 | 极端情况下（2448×2048 + 360° NCC = **2552 ms**）调用方无法设上限，节拍无法保证 |

### 6. Shape 的搜索层数不可控 —— 缺（NCC 有）

| 项 | 说明 |
|---|---|
| Halcon | `find_shape_model(..., NumLevels, ...)` 可在搜索时覆盖起始层 |
| 本项目 | `FindParams` **没有** `numLevels`（只有 `NccFindParams` 有）；`-levels` 只作用于建模 |
| 影响 | 想用「从更粗层开始」换速度、或「从更细层开始」换召回，只能重建模型 |

### 7. generic 参数机制 —— 缺

| 项 | 说明 |
|---|---|
| Halcon | `SetShapeModelParam` 接受字符串键（`min_size` / `max_size` / `timeout` …），版本升级不破坏 ABI |
| 本项目 | 参数是编译期结构体（`Config` / `MatchParams` / `FindParams`），新增参数要改头文件并重编调用方 |
| 影响 | 现场调试不灵活；SDK 升级对客户是破坏性的 |

### 8. 亚像素档位较少 —— 部分缺

| 项 | 说明 |
|---|---|
| Halcon | `SubPixel` 为字符串，除 `none` / `interpolation` / `least_squares` 外还有更高精度档位 |
| 本项目 | Shape：0 无 / 1 抛物线 / 2 高斯-牛顿最小二乘；NCC：只有 0/1（**无 least_squares**） |
| 影响 | 高精度测量场景可能不够；但本项目角度精度已优于 Halcon，优先级中等 |

### 9. 颜色信息 —— 缺

| 项 | 说明 |
|---|---|
| Halcon | 颜色形状模型 / `ignore_color_polarity` 度量的多通道模型 |
| 本项目 | `GeoMatch.cpp` 在建模与搜索两处都 `COLOR_BGR2GRAY`，**纯灰度引擎** |
| 影响 | 只有颜色区分（黑白同形）的目标无法区分 |

### 10. 形变 / 透视 / 标定匹配 —— 完全缺

| 项 | 说明 |
|---|---|
| Halcon | `HDeformableModel`（可变形匹配）、`HDescriptorModel`（描述子/透视）、`HCamPar::CreateCalibDescriptorModel`（标定透视匹配） |
| 本项目 | 无对应实现（既无形变自由度，也无描述子/单应） |
| 影响 | 柔性件、大倾角相机场景做不了 —— 是否补齐取决于业务，不是补丁级工作量 |

---

## 二、性能缺口（有实测数字）

> ⚠️ 下表是**旧构建**的数据；2026-11-16 晚用当前 `build\`（x64 AVX2）复测，
> data1 已从 44.7 ms 降到 **20.2 ms**、大图 Shape 从 251.8 ms 降到 **83.9 ms**，
> NCC 大图 **2.8 s → 0.35 s**（退层判据已落地，见 §9.2）。
> 复测明细与新发现的优化点见 **§九**。

| 场景 | 本项目 | Halcon 17 | 倍数 |
|---|---|---|---|
| data1 焊盘（140×140、±10°、1280×1024） | 44.7 ms | 7.9 ms | **5.7×** |
| data2 通孔（110×110、±100°） | 47.9 ms | 9.9 ms | 4.8× |
| 合成样本（360° × 49 尺度） | 314 ms | 21 ms | 15× |
| 多尺度螺母（0.4~1.7、360°） | 788 ms | 42.6 ms | 18× |
| 大图 2448×2048 + 360° Shape | 252 ms | 6.2 ms | **40×** |
| 大图 2448×2048 + 360° NCC | **2552 ms** | 32 ms | **80×** |
| data1 NCC（小角度） | 11.7 ms | 10.4 ms | 1.1×（同量级） |
| data2 NCC（±100°） | 20.8 ms | 24.5 ms | **0.85×（我们更快）** |

原因（`.clinerules` 里已有拆解）：
1. **评分内核**：粗扫已用 int8 定点 SIMD，但 refine / 亚像素 / rescore 仍走半精度标量，Halcon 全程 8 位整数 + SIMD；
2. **逐尺度卷积**：每个尺度重建旋转模板并整图相关，Halcon 的尺度网格有共享/预计算；
3. **NCC 旋转模板在搜索期构建**：模型文件极小（见 §五），但每张图都要重算；
4. **无 ROI / 无 border 预筛**：整图全扫；
5. **粗层点数随尺度放大**（`maxPoints=0` 时 level1 点数 ×10~20）。

---

## 三、精度 / 召回的残留差距

| 项 | 现状 | 判断 |
|---|---|---|
| Shape 分数水平 | 合成 0.903 vs Halcon 0.956（**−0.053**）；真实焊盘 0.961 vs 0.924（我们更高） | 阈值可迁移，不是功能缺陷 |
| **建模对比度默认值** | CLI 默认 12/30（≈灰度 3/7.5），Halcon 默认 30 灰度（≈我们 120）；data1 用默认只有 0.679，调到 120/160 才 0.961 | **默认值不自适应是真实短板** |
| 密集重复召回 | `rings_02`：我们 3 / Halcon 4（Halcon 第 4 个落在齿轮上，属误检） | 不算真差距；追召回调 `minScore` / `min-cover` |
| data2 漏检 | 我们漏 A 图、Halcon 漏 B 图（互补） | 没有绝对优势，属敏感点差异 |
| NCC 无尺度 | 80 张缩放样本：61/80（Halcon 64/80） | **算法维度**问题，Halcon NCC 同样没有；有尺度用 Shape |
| NCC 相似纹理错配分 | 0.70（Halcon 0.703） | 双方一致，属 NCC 性质；文档要强调 minScore 取值 |

---

## 四、工程化 / 可运维差距

| 项 | Halcon | 本项目 | 影响 |
|---|---|---|---|
| 错误体系 | `HException` + 数字错误码（#8510 / #3513 / #1301 …），可被程序分支 | `bool` + 文本日志（本轮刚补「输入不可读 → exit −1」） | 现场排障只能靠日志文本 |
| 语言绑定 | HALCON/Python、HALCON/.NET、C++、HDevEngine | 只有 C++ 头文件 + 静态/动态库（`docs/SDK.md`） | Python / 上位机集成要自己写 wrapper |
| 加速 | 自研 SIMD + 部分算子 GPU / 深度学习 | CPU AVX2/SSE2 + OpenCV OpenCL（本机 0 device，实际未用） | 大图吞吐差距的大头 |
| 参数化 | generic param 字符串，运行时可变 | 编译期结构体 | SDK 升级破坏兼容 |
| 回归测试 | 商业版内部，无公开可复现集 | 本轮新增 `tools/compare-dataset.ps1` + `_ai_tmp/extreme_cases.ps1`，**但没有 CI**；全量 `dataset/data1`(117) / `data2`(36) 未入库 | 改动容易悄悄回退（历史上已发生过） |
| 诊断 | `get_shape_model_params`、算子级计时 | `-debug 2`、`SM_PROBE` / `SM_DUMP_COS` / `SM_DUMP_CELLS` | 可用，但偏「内部调试」风格 |

---

## 五、本项目反而优于 Halcon 的地方（避免只列缺点）

| 项 | 数据 |
|---|---|
| NCC 模型体积 | data2 ±100°：**28 KB** vs Halcon **2.68 MB**；360° 154×148：**53 KB** vs **11.4 MB**（小 95~213 倍，适合嵌入式/多模型常驻） |
| 宽尺度范围 | 尺度 0.2~2：我们 **28.5 ms**，Halcon `ScaleStep=auto` 同区间 **15 279 ms** |
| 角度精度 | 合成真值平均误差 **0.068°** vs Halcon 0.113° |
| 小角度 NCC 速度 | data1 11.7 vs 10.4 ms（同量级）；data2 ±100° 我们 **20.8 ms 快于 24.5 ms** |
| 极端参数 | 67 例 × x64/x86 **0 崩溃**；Halcon 在「尺度 8..9」→ #3513、对比度 1e6 → #3100 直接拒绝 |
| 可改性 | 全源码；可加诊断开关、可定制评分口径（`scoreMode` / `skipWeight` / `cellMissMax` Halcon 没有） |

---

## 六、建议优先级

| 优先级 | 事项 | 理由 | 预估工作量 |
|---|---|---|---|
| **P0** | 搜索 ROI（`detect(gray, roi)` + CLI `-roi` + GUI 框「搜索区」） | 大图性能与误检同时受益，改动局限在 `prepareSource` / 候选提取 | 小（2~3 天） |
| **P0** | 建模对比度默认值自适应（按模板梯度分位数，或对齐 Halcon 灰度 30） | 默认参数下分数差 0.27，是最容易踩的坑 | 小（1 天） |
| **P0** | Shape 大图性能：refine/rescore 走 int8、行指针外提、按尺度复用梯度 | 40× 差距的主要来源；验收要求「结果逐位不变」 | 中（1~2 周） |
| **P1** | `FindParams::numLevels`（搜索层数覆盖） | 对齐 Halcon 语义，调速度/召回不必重建模型 | 小（1 天） |
| **P1** | 超时 / 可中断（`FindParams::timeoutMs` + 返回码） | 产线节拍硬需求；当前 2.5 s 无法限时 | 小（2 天） |
| **P1** | 多模型同搜 + 跨模型 NMS | 多品种共线场景 | 中（1 周） |
| **P1** | 错误码枚举（`bool` → `Status` / `ErrorCode`） | SDK 交付质量 | 小~中（3 天） |
| **P2** | NCC 旋转模板预计算/落盘缓存（代价：模型变大） | NCC 大图 80× 差距的直接原因 | 中（1 周） |
| **P2** | 从 XLD / 轮廓建模 | 离线 / CAD 场景 | 中（1~2 周） |
| **P2** | 各向异性缩放 | 透视 / 畸变场景 | 中（1 周） |
| **P2** | CI 回归（`dataset/sample` 全套 + 极端矩阵 + 双架构） | 防止历史问题回退 | 小（2 天） |
| **P3** | 颜色模型、Python/C# 绑定、形变/标定匹配 | 取决于业务是否出现这类需求 | 大 |

> 选型建议：**重复纹理 / 低纹理 / 大角度 / 小尺度范围 → NCC；
> 有尺度变化 / 大图 / 角度少 → Shape；有透视形变 → 两边都要另想办法（Halcon 有 descriptor/deformable，我们没有）。**

---

## 七、被证伪 / 不成立的「差距」（不要再花时间）

1. **贴边目标（部分出画）**：受控实验（`_ai_tmp/edge_case2.ps1`：300×200 画布、模板 140×140 故意左移 −40 px）
   —— 我们 **0 命中**，**Halcon 17 Shape 与 NCC 也都 0 命中**（Halcon 同时给出 `matches=0`）。
   两边都只搜「模板完整落在图内」的位姿（我们的 `scoreMap.create(rows - K + 1, cols - K + 1, ...)`
   就是这个含义）。所以这条**不是差距**；若现场真有需求，是要新增的功能
   （Halcon 新版本有 `border_shape_models`，本机 17 版这套调用未生效）。
2. **NCC 加尺度维度**：Halcon NCC 同样没有；需求应转给 Shape。
3. **Shape 分数绝对值追平 Halcon**：同实例 Δ 只有 0.003~0.016，追绝对值无收益；
   真正该修的是**默认参数**（对比度 / 点数）。
4. **靠 NMS 抑制重复纹理误检**：已实测证伪（假位姿几何上不重叠，`maxOverlap=0.3` 才掉）；
   正确工具是 `minCoverage` 与评分口径本身。

---

## 八、复现本文数据

```powershell
# 四器对比（检出率 / 分数 / 角度 / 耗时）
powershell -File tools\compare-dataset.ps1 -Set data1 -Template dataset\sample\data1\template_pad.jpg `
    -Dir dataset\sample\data1 -AngleStart -10 -AngleExtent 20 -RebuildModels -OutCsv _ai_tmp\ds_data1.csv
python _ai_tmp\ds_analyze.py

# 极端参数矩阵（x64 / x86）
powershell -File _ai_tmp\extreme_cases.ps1
powershell -File _ai_tmp\extreme_cases.ps1 -BuildDir build_x86 -Tag x86
powershell -File _ai_tmp\halcon_extreme.ps1

# 贴边目标（双方都 0 命中）
powershell -File _ai_tmp\edge_case2.ps1
```

---

## 九、2026-11-16 晚复测 + 新发现的优化点

> 口径：本项目 `build\`（x64 / AVX2）vs Halcon 17（x86 / SSE2）；时间 = 预处理 + 搜索；
> 同模板、同角度范围、同阈值。目的：§二 的耗时是旧构建，复测后**大图与 Shape 差距明显收窄，
> NCC 大图仍是短板**。

### 9.1 复测数据

| 场景（命令见 9.4） | 本项目 | Halcon 17 | 倍数 |
|---|---|---|---|
| data1 单张（1280×1024，±10°，模板 140×140，s=0.968） | pre 2.4 + search 17.8 = **20.2 ms** | **4.06 ms**（s=0.9625） | 5.0×（纯搜索 4.4×） |
| qfn1（2048×2448，m3 模板 102×102，360°，无目标）Shape | pre 16.4 + search 67.5 = **83.9 ms** | **8.38 ms**（0 命中） | 10× |
| qfn1 同上 NCC 360°（`-minscore 0.3`） | **2811 ms**（9 个 0.353~0.359 的假位姿） | **`#6001 Not enough memory`** | 32 位 Halcon NCC 在这张 5MP 图上直接拒绝 |

- 32 位 Halcon 17 的 NCC 在 2448×2048 上**内存不足**（与角度范围无关，`-angle-extent 20` 同样报错），
  所以 §二 里「Halcon NCC 32.1 ms / 80×」这一行**本机当前无法复现**，引用时要以「Halcon 64 位」为前提。

### 9.2 三个「可做、收益明确、能保证结果不变」的点

**(1) Shape 大图：候选提取（extract）占搜索时间 63%**

```
qfn1, m3 模板, 360°:  [dbg2] topScan=63.1 (extract=42.3) refine=2.3 subpixel=1.8  sum=67.2 ms
                      [dbg]  top=2 angles=260 scales=1 candidates=4121  poses=65
```

- 成因：65 个 (角度×尺度) 每个都要把 612×512 的 scoreMap **整幅再扫一遍**找 3×3 局部极大
  （≈20M 像素/图）；`v < thresh` 的提前返回只省了邻域比较，省不掉这次遍历。
- 改法（**位结果等价**）：
  1. 粗扫时在 `ScanInvoker8` / `ScanInvoker` 里顺带归约出 `max(scoreMap)`；
     `mapMax < topTarget` 的 map 直接跳过 `extractCandidates` —— 这种 map 本来就一个候选都不产出，
     属于纯剪枝，不需要 A/B 验证结果；
  2. 进一步把 3×3 极大判定融进扫描的**滚动 3 行缓冲**，省掉第二遍内存流量。
- 预期：qfn1 search 67.5 → **约 25 ms**，与 Halcon 差距 10× → 约 3×。
- 验收：`dataset/sample` 全量 + qfn1 的 `pos/angle/scale/score` 逐字符比对（必须完全一致）。

> **2026-11-16 实测：第 1 步（per-map max 剪枝）被证伪，不要按这个做。**
> 用现成的 `SM_PROBE=1` 探针统计 qfn1 上 65 个 pose 的分数图峰值：
> `poses=65  below0.3=0  below0.2=0   min 0.5 / mean 0.569 / max 0.7`，
> 而粗层 `topTarget = minScore*0.55 = 0.3`。也就是说**每个 pose 的 map 都有高于阈值的峰**
> （512×612 的位置里 102×102 的模板总能偶然对上若干处），`mapMax < thresh` 一个都剪不掉。
> 剩下的只能是第 2 步（把 3×3 极大判定融进扫描内核的滚动行缓冲）。
> 注意这需要处理并行 stripe 的边界行（现在 `stripes = rows/4`，overlap 一行就是 +25% 成本，
> 需同步调大 stripe 高度），且候选的输出顺序必须与现在一致（`std::sort` 不稳定，
> 分数并列时靠遍历顺序决定谁进 `maxPerMap` 截断），属于高风险改动。


**(2) NCC：无目标大图上的「退层重扫」把 67 ms 放大成 2.8 s**

```
qfn1（无目标）:  level3 coarse 61.4 ms  mapMax 0.4738 < 0.70 -> 退一层
                level2 coarse 237.4 ms  mapMax 0.4161 < 0.70 -> 再退
                ...  每层 20000 个 local maxima 全部进入 refine（5MP 图上 refine 是秒级）
```

- §7.3 那条「粗层峰值不可信就退一层」是为**噪声/弱对比场景救召回**加的，
  但它在「大图 + 本来就没有目标」时必然触发多次全量重扫 + 上万个候选精化。
- 改法（按风险从低到高）：
  1. 退层前先看**粗层候选数 / 已耗时**，超预算就不退（保留「退一层」的收益，去掉「连退三层」）；
  2. `mapMax` 与 `minScore` 差距很大时（例如 `mapMax < 0.6*minScore`）直接判 0 命中返回；
     Halcon 从不退层重扫，它就是直接返回 0。
- 预期：无目标 5MP 图 **2.8 s → < 0.3 s**；有目标场景必须用 `dataset/sample` + 噪声图 A/B 确认召回不掉。
- 注意：NCC 的 `minSide=9` 与新退层规则有耦合，改完必须复跑「PCB 重复焊盘」那个用例（0.789 错位回归）。

> **2026-11-16 已落地**（`NccMatch.cpp`，退层判据）：退层前要求「细一层的峰值比上一层**至少高
> `fallbackGain`**（默认 0.02）」，否则保留当前层候选直接进入 refine。
> `SM_NCC_FALLBACK_GAIN` 可覆盖该值，**负值 = 关闭护栏**（复现旧行为），便于 A/B。
>
> 实测（qfn1 2048×2448，360°，minScore 0.3，无目标）：
>
> ```
> 旧：level3 63 ms (0.4738) -> level2 256 ms (0.4161) -> level1 2272 ms (0.3844) -> refine
>     总 2619 ms
> 新：level3 63 ms (0.4738) -> level2 256 ms (0.4161，未改善) -> 停，用 level2 候选
>     总  347 ms     （x64 7.5×）
> x86：2755 ms -> 579 ms（4.8×）
> ```
>
> 召回 A/B（`_ai_tmp/ab_ncc.ps1`，105 张：data2 9 + data1 7 + case1 9 + synth 80）：
> `Compare-Object` 只有 `load xx ms` / `avg xx ms/image` 两行时间差，
> **每张图的 hits / score / pos / angle 逐字符一致**；qfn1 连跑 3 次结果也完全一致。
> 批量场景还更快：synth 80 张 18.96 → **8.26 ms/张（2.3×）**，data1 3.66 → 3.25，case1 28.19 → 27.67。


**(3) 结果侧对外接口缺三样（接上位机 / 机器人时最先撞到）**

> **2026-11-16 已落地前两项**（`GeoMatch.h` / `NccMatch.h` 的「pose & geometry」段落）：
> `poseMatrix` / `poseAffine` / `cornersAt` / `boundsAt` / `contourAt`（仅 Shape）/
> `templateToImage` / `imageToTemplate` / `setOrigin`·`resetOrigin`·`origin`。
> 统一约定：`imagePt = pos + s·R(angle)·(tplPt − origin())`，`R = [cos,sin;−sin,cos]`。
> `setOrigin` 取**模板绝对像素坐标**（Halcon 是相对中心的偏移，= 中心 + 偏移）。
> 已用 `_ai_tmp/pose_selftest.cpp` 在 data1 与 synth（135°/−45°、0.7 缩放）上验证：
> 中心映射误差 0、正逆变换 < 3e-5 px、四角边长 = 尺寸×scale、换 origin 后 score/angle/四角不变。
> 文档见 `docs/SDK.md` 5.6（含每个接口的参数表、Halcon 对应、自检方法），
> 可运行示例 `examples/example_host_integration.cpp`（参与 CMake 构建）。
> **还剩多模型同搜**未做。

| 缺什么 | Halcon 对应 | 我们的现状 | 影响 |
|---|---|---|---|
| 自定义参考点 | `set_shape_model_origin` / `set_ncc_model_origin` | 固定返回模板中心（`modelLevels_[0].center = (cols/2, rows/2)`） | 机器人 TCP / 贴装偏置要调用方换算；从 Halcon 迁移时结果差半个模板 |
| 位姿矩阵 + 轮廓 | `get_shape_model_contours` + `affine_trans_contour_xld` | 只有 `drawContours(dst, m, ...)`（画在图上的位姿版）与 `getDrawResult()`（返回**模型点散点**，不是轮廓折线） | 下游测量/绘制拿不到「位姿下的轮廓多边形」，只能自己反算 |
| 多模型同搜 | `find_shape_models`（模型数组、跨模型统一 NMS、共享预处理） | 单模型单对象，多模板只能循环调用 | 大图 preprocess 已占 16.4/83.9 ms，多品种共线会重复 N 次；跨模型重叠还要外部再滤 |

建议先补两个小函数（1~2 天，不动算法）：
`std::vector<std::vector<cv::Point2f>> contourAt(const Detection&) const`（复用已有的 level-0 `chains`）
与 `cv::Matx33d poseMat(const Detection&) const`（`[s·cosθ, s·sinθ, tx; -s·sinθ, s·cosθ, ty; 0,0,1]`）。

### 9.3 其他低成本项（顺手可做）

1. **`metric=2`（ignore_local_polarity）走标量扫描**（`scanLocalPolarity`），
   可改成 `|dot|` 的 `vpmaddwd` 向量化；Halcon 侧这类参数几乎不掉速。
2. **`useOpenCL` 默认 `true`**，但本机 `haveOpenCL=1 / 0 device`，实际没用上；
   建议启动时探测，0 device 就静默关闭并打印一行，避免现场误以为「已开 GPU 加速」。
3. **NCC 没有 Shape 那套覆盖率闸门**（`minCoverage` / `cellMissMax`）：重复纹理误配只能靠抬高 `minScore`
   （实测相似纹理错配能到 0.70）。NCC 覆盖率可用「模板像素中图像梯度 > 阈值的占比」定义，成本低。
4. **默认值口径与 Halcon 不一致**：Shape `maxOverlap` 默认 1.0（Halcon 0.5）、`minScore` 默认 0.7（Halcon 0.5）、
   NCC `minScore` 默认 0.7（Halcon 0.5）。迁移参数/写对标报告时要显式说明，否则会被当成精度差。
5. **`Config` 同时承载建模与搜索参数，`load()` 之后会被模型内参数覆盖**：
   推荐姿势固定写成 `load()` → `config()` → 改 → `setConfig()` 三步，否则现场最容易出「改了参数没生效」。

### 9.4 复测命令（可直接重跑）

```powershell
# Shape 小图（data1 单张）
build\GeoMatchDemo.exe -t dataset\sample\data1\template_pad.jpg `
    -d dataset\sample\data1\scan_1783730078944_score.jpg `
    -angle 20 -contrast 120 -hi 160 -minscore 0.5 -max 1 -debug 2 -noshow -o _ai_tmp\probe2

# Shape 大图（qfn1，2048×2448，360°；看 [dbg2] 的 topScan/extract 拆分）
build\GeoMatchDemo.exe -t dataset\image\m3.bmp -d dataset\image\qfn1.bmp `
    -angle 360 -minscore 0.5 -max 12 -debug 2 -noshow -o _ai_tmp\probe

# NCC 大图（两步：先建模型再搜）
build\NccDemo.exe -t1 -t dataset\image\m3.bmp -save _ai_tmp\m3.ncm -angle 360
build\NccDemo.exe -load _ai_tmp\m3.ncm -s dataset\image\qfn1.bmp -o _ai_tmp\probe3 `
    -minscore 0.3 -max 12 -debug 2 -nodraw

# Halcon 17 对照（x86，需先设 PATH / HALCONARCH / HALCONROOT）
$env:PATH = (Join-Path (Get-Location) 'HalconDemo\Halcon17\bin') + ';' + $env:PATH
$env:HALCONARCH = 'x86sse2-win32'; $env:HALCONROOT = (Join-Path (Get-Location) 'HalconDemo\Halcon17')
HalconDemo\halcon_shape_compare.exe dataset\sample\data1\template_pad.jpg `
    dataset\sample\data1\scan_1783730078944_score.jpg -angle-start -10 -angle-extent 20 -minscore 0.3 -max 1 -loop 5
HalconDemo\halcon_shape_compare.exe dataset\image\m3.bmp dataset\image\qfn1.bmp `
    -angle-start -180 -angle-extent 360 -minscore 0.5 -max 12 -loop 3
HalconDemo\halcon_ncc_compare.exe   dataset\image\m3.bmp dataset\image\qfn1.bmp `
    -angle-start -180 -angle-extent 360 -minscore 0.3 -max 12     # 本机报 #6001
```

### 9.5 更新后的优先级（覆盖 §六）

| 优先级 | 事项 | 依据 | 预估 |
|---|---|---|---|
| **P0** | ~~粗扫 extract 剪枝（per-map max）~~ → 只剩「滚动 3×3 融合进扫描内核」 | per-map 剪枝实测证伪（65/65 pose 峰值 ≥0.5）；滚动融合是高风险重构 | 中（1 周） |
| **P0** | ✅ 已完成：NCC 退层只在峰值改善时继续 | 无目标 5MP 图 2619 → **347 ms**（x64 7.5×、x86 4.8×），105 张图结果逐字符不变 | 已完成 |
| **P0** | 搜索 ROI（`detect(gray, roi)` + CLI `-roi` + GUI「搜索区」） | 大图/多工位通用收益，与 extract 剪枝叠加 | 小（2~3 天） |
| **P0** | 建模对比度自适应（对齐 Halcon 灰度 30 的口径） | 默认参数下分数差 0.27 | 小（1 天） |
| **P1** | `FindParams::numLevels`（搜索层数覆盖） | 调速度/召回不必重建模型 | 小（1 天） |
| **P1** | 结果接口：`contourAt()` + `poseMat()` + 自定义 origin | 上位机/机器人对接必需 | 小（2 天） |
| **P1** | 超时/可中断（`FindParams::timeoutMs` + 返回码） | 产线节拍硬需求 | 小（2 天） |
| **P1** | 多模型同搜 + 跨模型 NMS + 共享预处理 | 多品种共线；大图预处理不重复 | 中（1 周） |
| **P1** | 错误码枚举（`bool` → `Status`） | SDK 交付质量 | 小~中（3 天） |
| **P2** | `metric=2` 向量化 / NCC 覆盖率闸门 / OpenCL 自动探测 | 各自 1 天内 | 小 |
| **P2** | NCC 旋转模板落盘缓存、按角度紧包围盒画布 | NCC 大图剩余差距 | 中 |
| **P2** | CI 回归（`dataset/sample` + 极端矩阵 + 双架构） | 防止历史问题回退 | 小（2 天） |
| **P3** | 各向异性缩放、XLD 建模、颜色、形变/透视、Python·C# 绑定 | 取决于业务 | 大 |





