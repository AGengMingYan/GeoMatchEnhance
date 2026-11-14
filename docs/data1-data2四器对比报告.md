# data1 / data2 四器对比评估报告（2026-11-14）

> 数据集：`dataset/data1`（117 张，1280×1024 芯片裸片）、`dataset/data2`（36 张，1280×1024 蛇形走线）
> 对比对象：本项目 Shape（x64 AVX2）、本项目 NCC（x64 AVX2）、Halcon 17 Shape（**x86 SSE2**）、Halcon 17 NCC（x86 SSE2）
> 采集脚本：`tools/compare-data.ps1`　汇总脚本：`tools/analyze-data.ps1`、`tools/angle-agreement.ps1`
> 原始结果：`_ai_tmp/data1.csv`、`_ai_tmp/data2.csv`（`_ai_tmp` 不入库，重跑即得）
>
> **⚠️ 2026-11-14 晚间重要更新：Shape 分数差距的根因已找到并修复（`skipWeight`）。**
> 修复后 Shape 与 Halcon 的分数差距从 ~0.30 缩到 **0.05~0.07**，data2 的 −90° 簇检出
> 从 0/18 反超 Halcon 到 **17/18**（Halcon 11/18）。详见 **第九节**；
> 第七节的"根因分析"保留为修复前的排查记录（其中"外观差异无解"的结论已被第九节推翻）。
>
> **最新一次复测（修复后、同模板重建模型、抽样 95 张）见 `docs/抽样对比-2026-11-14.md`。**

---

## 〇、前提与口径（必读）

1. **架构不对等**：Halcon 侧本机只有 32 位 SSE2（`HalconDemo/Halcon17/x86sse2-win32`），
   本项目是 64 位 AVX2。所有速度结论都带这个前提，不能当作纯算法优劣。
2. **时间口径**：本项目耗时 = `preprocess + search`；Halcon 耗时 = `find_*` 仅搜索。
3. **重复目标场景**：两个数据集的画面里都有十几个高度相似的重复目标（焊盘/通孔）。
   用 `max=1` 只取 top-1 时，不同匹配器可能选了**不同的物理实例**，
   所以**位置差 dPos 在这种场景下不具参考意义**（会出现几百 px 的"假误差"）。
   真正可靠的指标是：**角度差 dAngle**（同类目标朝向一致）、**检出率**、**同实例（dPos<15px）分数差**。
4. 统一参数：minScore=0.5、max=1、maxOverlap=0.5、use_polarity、subpixel 开、金字塔自动。

---

## 一、模板与模型（两个数据集目标完全不同，各自独立建模）

| 数据集 | 目标 | 模板 | 尺寸 | 角度范围 | 实测目标朝向 |
|---|---|---|---|---|---|
| data1 | 白色方形焊盘（内部带暗点） | `dataset/data1/template_pad.jpg` | 140×140 | **±10°** | 全部在 ±1.1° |
| data2 | 蛇形走线上的通孔标记（白方块+暗孔） | `dataset/data2/template_via.jpg` | 110×110 | **±100°** | 双峰：0°（18 张）/ −90°（18 张） |

- 模板用 PowerShell `System.Drawing` 从首图裁剪（本机加密环境下保证明文，Halcon 读密文会 abort）。
- 角度范围不是拍脑袋：先用 360° NCC 模型对全量图做角度普查，
  data1 角度全落在 −1.1°..0.1°，data2 干净地分成 0° 和 −89° 两簇、各 18 张、无其他角度，
  因此 data1 用 ±10°、data2 用 ±100°（覆盖两簇且比 360° 省）。
- 模型点数：data1 shape 1200 点 / data2 shape 970 点（Halcon 内部点数工具未输出）。
- 模型文件（`.shm/.ncm` 被 .gitignore 忽略，可用第十节命令重建）：
  本项目 `build/_data1.{shm,ncm}`、`build/_data2.{shm,ncm}`；
  Halcon `HalconDemo/_data1.{shm,ncm}`、`HalconDemo/_data2.{shm,ncm}`。

---

## 二、总体结论（TL;DR）

| 维度 | NCC（本项目 vs Halcon） | Shape（本项目 vs Halcon） |
|---|---|---|
| 检出率 | **打平**（data1 117/117、data2 36/36） | **本项目明显落后**（见下） |
| 同实例分数差 | **0.0014~0.0017，几乎完全等价** | 系统性低 **0.30**（data1） |
| 角度一致性 | mean ≤0.1°（同朝向时） | mean 0.19~0.25°，同样很准 |
| 大角度（−90°） | **稳定** | **本项目 0/18 漏检；Halcon 11/18，另 7 张 Halcon 也检不出** |
| 速度（data1） | **本项目 6.42 ms 快于 Halcon 8.61 ms** | 本项目 28.9 ms，Halcon 4.6 ms |
| 速度（data2） | 本项目 20.1 ms，略慢于 Halcon 16.5 ms | 本项目 23.1 ms，Halcon 8.6 ms |

> data2 速度对比受角度范围（±100°、201~262 个角度）影响很大，且 Halcon NCC 的
> 模型文件高达 2.67 MB（预计算量大）。下面分节给详细数字。

---

## 三、检出率

| 数据集 | N | NCC ours | NCC Halcon | Shape ours | Shape Halcon |
|---|---|---|---|---|---|
| data1 | 117 | **117** | **117** | **117** | **117** |
| data2 | 36 | **36** | **36** | **5** | 29 |

### data2 按真实朝向拆分（真值取 NCC 角度，±15° 容差）

| 朝向簇 | 张数 | 本项目 Shape | Halcon Shape |
|---|---|---|---|
| 0° 簇 | 18 | 5 | **18** |
| **−90° 簇** | 18 | **0（全部漏检）** | 11 |

> data2 的 −90° 簇是本项目 Shape 最严重的问题：**18 张全部漏检**。
> 0° 簇也只检出 5/18（低分图被 minScore=0.5 截掉）。Halcon Shape 在 0° 簇全中、−90° 簇中 11/18。

---

## 四、分数（仅统计检出项）

### data1（117 张全部检出）

| 匹配器 | min | p10 | p50 | p90 | max | mean |
|---|---|---|---|---|---|---|
| Shape ours | 0.505 | 0.582 | 0.616 | 0.624 | 0.977 | 0.617 |
| Shape Halcon | 0.622 | 0.924 | **0.962** | 0.965 | 0.995 | **0.947** |
| NCC ours | 0.930 | 0.953 | 0.974 | 0.983 | 1.000 | 0.972 |
| NCC Halcon | 0.939 | 0.970 | 0.979 | 0.987 | 1.000 | 0.979 |

### data2

| 匹配器 | 检出数 | min | p50 | max | mean |
|---|---|---|---|---|---|
| Shape ours | 5 | 0.881 | 0.893 | 0.988 | 0.911 |
| Shape Halcon | 29 | 0.568 | 0.627 | 0.990 | 0.693 |
| NCC ours | 36 | 0.624 | 0.846 | 1.000 | 0.837 |
| NCC Halcon | 36 | 0.624 | 0.865 | 1.000 | 0.855 |

### 同实例分数差（dPos<15px 才算同一个物理目标，规避重复目标 top-1 不同）

| 数据集 | NCC 同实例对数 | NCC 分数差 mean/max | Shape 同实例对数 | Shape 分数差 mean/max |
|---|---|---|---|---|
| data1 | 85 | **0.0017 / 0.0022** | 24 | **0.3044 / 0.3563** |
| data2 | 32 | **0.0014 / 0.0022** | 5 | 0.0640 / 0.0911 |

**结论**：

- **NCC 两器在同一物理目标上的分数差只有 0.001~0.002，可视为完全等价。**
- 本项目 Shape 在 data1 焊盘上**分数系统性低约 0.30**（同一焊盘：ours 0.619 vs Halcon 0.957 vs NCC 0.985）。
  位置/角度其实是对的（见第五节），纯粹是**评分标定偏保守**。

---

## 五、角度一致性（重复目标场景下最可信的一致性指标）

| 数据集 | Shape `|dAngle|` mean/p90/max | NCC `|dAngle|` mean/p90/max |
|---|---|---|
| data1 (n=117) | **0.186° / 0.300° / 0.520°** | **0.096° / 0.170° / 0.280°** |
| data2 (n=5 / 36) | 0.246° / 0.330° / 0.380° | 同朝向时 p90=1.2° |

- 只要两器都检出，**角度都对得很准**（Shape 平均偏差 <0.25°，NCC <0.1°）。
- data2 NCC 的 mean=10.7° 是被 4 张"同时含 0° 和 −90° 两种通孔"的图拉高的：
  两器各选了一个朝向的实例（ours 选 0°、Halcon 选 −90°），角度差 ~93°，
  **属于 top-1 实例选择差异，不是角度算错**。剔除这 4 张后 p90 仅 1.2°。


---

## 六、速度（ms/张，全部图）

> 本项目含 preprocess+search；Halcon 仅 find。架构 x64 AVX2 vs x86 SSE2。

### data1（±10°，21 个角度，目标小而多）

| 匹配器 | p10 | p50 | p90 | mean |
|---|---|---|---|---|
| Shape ours | 27.30 | 28.70 | 30.20 | 28.91 |
| Shape Halcon | 3.88 | 4.40 | 5.85 | **4.61** |
| NCC ours | 5.75 | 6.17 | 7.19 | **6.42** |
| NCC Halcon | 7.49 | 8.35 | 10.24 | 8.61 |

### data2（±100°，201~262 个角度，角度搜索量大）

| 匹配器 | p10 | p50 | p90 | mean |
|---|---|---|---|---|
| Shape ours | 20.80 | 22.60 | 24.80 | 23.09 |
| Shape Halcon | 4.80 | 6.69 | 13.86 | **8.64** |
| NCC ours | 17.93 | 19.84 | 21.87 | **20.05** |
| NCC Halcon | 12.70 | 15.59 | 21.74 | 16.52 |

**解读**：

- **Shape 速度 Halcon 明显领先**（data1 约 6.3×、data2 约 2.7×）。但注意两边模型点数/内部优化
  不可见，且架构不同，这个倍数只能当"工程实测参考"。
- **NCC 在小角度范围（data1）本项目反超 Halcon（6.42 vs 8.61 ms）**；
  在大角度范围（data2，201 个角度）本项目 20.05 ms 略慢于 Halcon 16.52 ms。
- 本项目 NCC 的旋转模板跨帧 LRU 缓存（见 `docs/NCC-vs-Halcon.md` 第六节）在视频连续帧、
  角度变化小时收益最大；本评估每张都是独立进程冷启动，缓存命中率低，未体现热帧加速。

---

## 七、两个 Shape 缺陷的根因分析

### 缺陷 1：data1 焊盘分数系统性偏低（定位正确，分数低 ~0.30）

- 同一物理焊盘上四方结果：本项目 Shape 0.619 / Halcon Shape 0.957 / 本项目 NCC 0.985 / Halcon NCC 0.985，
  位置差 <0.5px、角度差 <0.25° —— **找得准，只是打分低**。
- 试过 4 种 scoreMode：mean(0)=0.619、positive-mean(1)=0.877、mode2=0.692、mode3=0.644，
  最高的 mode1 仍比 Halcon 低 0.08，且 coverage 只有 0.74。
- **根因**：焊盘是大面积均匀白色块，内部几乎没有梯度，模型特征点主要集中在方块边缘；
  本项目 Shape 的梯度方向点积评分在"低纹理、强极性块"上天然保守，而 Halcon 的归一化更宽松。
- 重建模型（2000/4000 点、不同对比度）不能改变这个标定差距。
- **影响**：若用本项目 Shape 且 minScore 设到 0.7 以上，data1 会大面积漏检；建议这类目标优先用 NCC。

### 缺陷 2：data2 的 −90° 朝向目标全部漏检（根因已在 2026-11-14 补查修正）

- 现象：18 张 −90° 真实图，本项目 Shape **0/18 检出**；其 top-1 反而落在 ~7° 的错误位姿、分数 0.40。
- **排除"不支持 90°"**：用模板自身旋转合成图测试，本项目 Shape 在 +90° 得 0.968、−90° 得 0.934，
  位姿都正确 —— 算法本身能处理直角。
- **先前的猜测（已证伪）**：曾在 `ShapeMatch.cpp` 里加过"角度桶多样性配额"，假设是
  "高对称目标在错误朝向上粗层分更高，把真实大角度位姿挤出 top-N"。实测结果：
  **输出与旧版逐位相同（36/36 一致），只让该图 search 从 24 ms 涨到 46 ms**，
  该启发式已删除（保留原先的全局按分截断）。
- **真正的根因（2026-11-14 对照实验）**：**模板与目标不是同一个实例的外观**。
  放大对照可见：模板（以及 0° 簇目标）是"单暗孔焊盘"，
  而 −90° 簇的目标是"**双暗孔 + 表面斑纹**"的另一种通孔 —— 形状分天然低。
  验证方法（可复现）：把模板按真实朝向旋转 ±90° 另建一个 ±10° 小角度模型
  （`_ai_tmp/rot_tpl.ps1` → `build/_data2p90.shm`），再对 18 张 −90° 图跑一遍：

  | | 本项目（对齐模板） | Halcon Shape | 说明 |
  |---|---|---|---|
  | 有检出的 11 张，真值处得分 | **0.29 ~ 0.36** | 0.57 ~ 0.68 | 即使位姿完美对齐，我们也只有 0.3 |
  | 两器都无检出的 7 张 | 无 | **无（Halcon 也没有）** | 这几张数据本身就没有可用目标 |

  即"**正确位姿本身就只能拿 0.3 分**"，与候选排序无关；
  Halcon 之所以 0.58 能过 0.5 的阈值，是因为它的 Shape 评分整体比我们乐观 ~0.3
  （与缺陷 1 的 data1 焊盘完全同一个量级）。
- 旁证：合成图上 135°/180° 本项目 Shape 也崩（报 −44°/90°、分数 0.32~0.37），
  而 Halcon 同图仍能定位（0.68~0.69）。说明本项目 Shape 的**大角度鲁棒性整体弱于 Halcon**。
- 对比：**NCC 两器在 −90° 簇全部稳定命中**（分数 0.62~0.93）——
  NCC 对"亮度结构相同、细节纹理不同"的实例更宽容，所以这类目标应该用 NCC。

### 给使用方的建议

1. 重复、低纹理、强极性的块状目标 → **优先 NCC**（本项目 NCC 与 Halcon 等价且小角度更快）。
2. 需要大旋转角（尤其接近 ±90°）、且目标实例外观可能与模板不一致（孔位/纹理有差异）→
   **用 NCC**。本项目 Shape 在这类目标上的分数上限只有 0.3 左右，任何 minScore ≥0.5 都会漏检。
3. 小角度（±10°）且边缘特征丰富、实例外观稳定的目标，本项目 Shape 的角度精度（mean 0.19°）
   和定位是可靠的。

> **排查方法论（下次遇到"该找到却找不到"时按这个顺序）**：
> 1. 先从 `-debug 2` 看粗层 `mapMax` 与候选数量，确认搜索链路本身跑通了；
> 2. **再用"按真实朝向预旋转的模板"另建一个小角度模型**去跑同一张图 ——
>    这一步能把"候选排序丢了位姿"和"这个位姿本身就只能拿低分"彻底分开；
> 3. 若对齐模板也只能拿低分，别再改搜索代码，去对比模板与目标实例的外观（放大截图）；
> 4. 顺便看 Halcon 在同样图上有没有检出 —— 它也没有的图直接跳过，不要浪费时间。

---

## 八、复现步骤

```powershell
# 0) Halcon 32 位环境
$env:PATH       = "$PWD\HalconDemo\Halcon17\bin;$env:PATH"
$env:HALCONARCH = 'x86sse2-win32'
$env:HALCONROOT = "$PWD\HalconDemo\Halcon17"

# 1) 解压数据集（7z 内是明文 jpg，解压后文件头 FF D8 FF）
& 'C:\Program Files\7-Zip\7z.exe' x dataset\data1.7z -odataset -y
& 'C:\Program Files\7-Zip\7z.exe' x dataset\data2.7z -odataset -y

# 2) 裁模板（System.Drawing 保证明文）；或直接用已入库的模板 jpg
powershell -ExecutionPolicy Bypass -File _ai_tmp\crop_tpl.ps1

# 3) 建模型（本项目 x64）
build\GeoMatchDemo.exe -t1 -t dataset\data1\template_pad.jpg -save build\_data1.shm -angle 20
build\NccDemo.exe      -t1 -t dataset\data1\template_pad.jpg -save build\_data1.ncm -angle-start -10  -angle 20
build\GeoMatchDemo.exe -t1 -t dataset\data2\template_via.jpg -save build\_data2.shm -angle 200
build\NccDemo.exe      -t1 -t dataset\data2\template_via.jpg -save build\_data2.ncm -angle-start -100 -angle 200

# 4) 建 Halcon 模型（32 位，-save-model 只建不搜）
HalconDemo\halcon_shape_compare.exe dataset\data1\template_pad.jpg -angle-start -10  -angle-extent 20  -save-model HalconDemo\_data1.shm
HalconDemo\halcon_ncc_compare.exe   dataset\data1\template_pad.jpg -angle-start -10  -angle-extent 20  -save-model HalconDemo\_data1.ncm
HalconDemo\halcon_shape_compare.exe dataset\data2\template_via.jpg -angle-start -100 -angle-extent 200 -save-model HalconDemo\_data2.shm
HalconDemo\halcon_ncc_compare.exe   dataset\data2\template_via.jpg -angle-start -100 -angle-extent 200 -save-model HalconDemo\_data2.ncm

# 5) 采集（data1 117 张建议分 3 批避免单次超时；data2 一次跑完）
powershell -ExecutionPolicy Bypass -File tools\compare-data.ps1 -Sets data2
powershell -ExecutionPolicy Bypass -File tools\compare-data.ps1 -Sets data1 -Offset 0  -Take 40
powershell -ExecutionPolicy Bypass -File tools\compare-data.ps1 -Sets data1 -Offset 40 -Take 40 -Append
powershell -ExecutionPolicy Bypass -File tools\compare-data.ps1 -Sets data1 -Offset 80 -Take 40 -Append

# 6) 汇总
powershell -ExecutionPolicy Bypass -File tools\analyze-data.ps1
powershell -ExecutionPolicy Bypass -File tools\angle-agreement.ps1
```

---

## 九、skipWeight 修复：Shape 分数差距的根因与结果（2026-11-14 晚）

### 9.0 后续修复：skipWeight 带来的"高分误检"（同日修复）

`skipWeight` 让模糊帧的分数对齐了 Halcon，但也**抬高了"只对上模板一部分"的假目标**。
决定性实验：模板只粘上 55% 到灰底图 → `skip-w 0.9` 报 **0.971**（cover 0.99），
`skip-w 0` 报 0.475（cover 0.49）。

- 同时修正：`coverage` 恢复为"一致点数 / **全部**模型点"（一度改成除以保留点数，
  导致 `minCoverage` 失效）；
- 新增 `cellMissMax`（默认 0.35）：模板切 4×4 格，统计"图像上没梯度"的格占比，
  超过阈值即拒绝。实测校准：清晰真目标 0.00、**模糊真目标 0.25**、
  data2 −90° 0.00~0.13、**模板只贴一半 0.50**；
- 判据前移到候选筛选（拒掉一个继续取下一个），避免"被拒的 top-1 让整张图变成 0 命中"。

修复后回归：data1 **117/117**（mean 0.886，gap 0.059）、data2 0°/ −90° **18/18、17/18**、
负样本 max 0.443、模板只贴一半 → 0 命中。

### 9.1 差距的精确定位（怎么找到的）

1. **合成"完美贴回"对照**（模板原样贴回灰图，无旋转）：两边都 ≈1.0
   （ours 0.983 / Halcon 0.9935）→ **评分公式本身等价**。
2. 但同一批真实图上 Halcon 只衰减 0.036（0.9935→0.957），我们衰减 0.364（0.983→0.619）
   → 我们对**真实扰动（模糊/弱对比/噪声）的敏感度是 Halcon 的 10 倍**。
3. **逐点 cos 分布**（新增 `SM_DUMP_COS=1` 诊断）：
   - 清晰帧：1200 个点里 1178 个 cos ≥ 0.9 —— 没问题；
   - **模糊帧：双峰**——62.5% 的点 ≈0.97+，**22.8% 的点塌到 0~0.17，9.2% 为负**。
4. 根因：归一化梯度平面存的是 `w·单位方向`，`w = min(|grad|/minContrast, 1)`。
   模糊把一部分模型点位置的幅值压过 minContrast，这些点的 dot 被 **w 等比衰减后计入
   mean** —— 1/3 的点 × ~0.1 权重 ≈ −0.3。
   **Halcon 的语义是"图像梯度幅值低于 MinContrast 的点不参加评分，分母同步缩小"**
   —— 所以强点平均 0.96 就报 0.96。

### 9.2 修复内容

`FindParams::skipWeight`（默认 **0.9**，0 = 关闭恢复旧语义）：
评分时跳过"采样梯度长度（= 软权重 w）< skipWeight"的点，mean/clipped/coverage 的
分母都改为**有效点数**。已实装进**全部评分路径**：SIMD 粗扫内核（AVX2/SSE2/标量，
per-lane mask + 计数）、`scorePos`（逐层精化/最小二乘/probe）、`scorePosSubpix`（抛物线
亚像素）、`scorePoseDetail`（最终 rescore）、`scanLocalPolarity`（metric=2）。
防御：一个位置的有效点数 < max(8, n/4) 时直接判 −1（不可靠）。

CLI：`GeoMatchDemo -skip-w 0.9`（默认）/ `-skip-w 0` 关闭。
Facade `Config::skipWeight` 同步默认 0.9，GUI/SDK 不改代码即继承。

### 9.3 修复前后（同模型、同参数、同一批图）

**data1（117 张，焊盘）—— 分数分布（top-1）**

| | min | p10 | p50 | mean | 与 Halcon mean 差 |
|---|---|---|---|---|---|
| 修复前 | 0.505 | 0.582 | 0.616 | 0.617 | **−0.330** |
| **修复后** | **0.680** | 0.838 | 0.880 | **0.873** | **−0.074** |
| Halcon | 0.622 | 0.924 | 0.962 | 0.947 | — |

- 检出仍 117/117；同实例相关性 0.568 → **0.808**；
- 我们的最小分 0.680 **已高于 Halcon 的最小分 0.622**（模糊最重的帧不再是我们的短板）。

**data2（36 张，通孔）—— 按真实朝向分簇（检出阈值 0.5）**

| 朝向簇 | 修复前 | **修复后** | Halcon |
|---|---|---|---|
| 0°（18 张） | 5/18，0.911 | **18/18，0.707** | 18/18，0.739 |
| **−90°（18 张）** | **0/18（全漏）** | **17/18，0.614** | 11/18，0.619 |

- **−90° 簇反超 Halcon**：那 7 张"Halcon 也返回无检出"的图，我们现在能以 0.53~0.69 检出
  （对称实例的差异在 skip 语义下被正确弱化）；只有 `scan_1776334904303` 这 1 张我们仍漏
  （Halcon 0.606）。
- 之前的"按真实朝向预旋转模板也只能拿 0.3"的说法**已被推翻**——低分不是外观差异，
  而是评分语义把"实例差异点"过度加权；修复后全局模型直接检出。

**误报安全性（负样本集：data1 模板 × data2 图 36 张，目标不存在）**

| | mean | p90 | max | ≥0.5 张数 |
|---|---|---|---|---|
| 修复前 | 0.425 | 0.436 | 0.444 | 0 |
| **修复后** | **0.338** | 0.390 | **0.403** | **0** |

假分数不升反降，真/假间隔（mean）从 0.19 拉大到 **0.54** —— 判别力同步增强。

**速度**：data1 mean 28.9 → **32.4 ms**（+12%，SIMD 内核多了 per-lane mask/计数）；
data2 shape mean 23.1 → 约 25 ms（−90° 簇现在会走完整精化，检出多了自然多花点时间）。
Halcon 参照 4.6/8.6 ms（32 位）不变。

**确定性**：同一模型连跑 3 次，分数/位置/角度逐位一致（0.838 @ 925.08,156.52, −1.24°）。

### 9.4 对「怎么选」的更新

- 第七节的选型建议依然有效（重复/低纹理/大角度优先 NCC），但 **Shape 不再对这类目标
  "不可用"**：minScore 可以按 Halcon 的口径（0.5~0.6）直接设定，分数可与 Halcon 直接对标
  （差 0.05~0.07）。
- 剩余差距（~0.07）的来源：模型点选择（我们 1200 点配额会收弱边点，Halcon 内部点数
  未知）、梯度算子细节差异。优先级不高。

### 9.5 复现

```powershell
# 修复后（默认）逐图分数（对照 _ai_tmp/data1.csv 里的 halcon 列）
_ai_tmp\reshape.ps1 -Set data1 -Take 40 -CsvOut _ai_tmp\data1.new.csv
_ai_tmp\reshape.ps1 -Set data1 -Offset 40 -Take 40 -Append
_ai_tmp\reshape.ps1 -Set data1 -Offset 80 -Take 40 -Append
_ai_tmp\data1_eval.ps1                       # 分数分布 + 相关性 + gap
_ai_tmp\reshape.ps1 -Set data2 -CsvOut _ai_tmp\data2.new.csv
_ai_tmp\data2_join.ps1                       # 按朝向分簇对比
# 关闭修复（恢复旧语义）对照：任何 t2 命令加 -skip-w 0
```

---

## 十、与 test2 评估的对照

| | test2（菱形标定块） | data1（焊盘） | data2（通孔） |
|---|---|---|---|
| 含目标图比例 | 10/200 | 117/117（每张都有焊盘） | 36/36 |
| NCC vs Halcon 相关 | 0.998 | 同实例分数差 0.002 | 同实例分数差 0.001 |
| Shape vs Halcon 相关 | 0.976 | 修复前 0.568 / **修复后 0.808**，gap 0.33→0.07 | 修复前 −90° 全漏 / **修复后 17/18** |
| 主要发现 | 双方高度一致 | Shape 评分保守（**已修复：skipWeight**） | **大角度缺陷（已修复）** |

test2 上 Shape 与 Halcon 相关性很好（0.976），但那是边缘特征清晰的菱形块；
data1/data2 暴露的问题**共同根因是"弱梯度点被软权重过度惩罚"**（第九节），
修复后 Shape 在低纹理块状目标与高对称大角度目标上都接近 Halcon；
NCC 在三个数据集上始终与 Halcon 保持等价。

