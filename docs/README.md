# 文档索引

> 本目录有 20+ 份文档，按**用途**分四类。拿不准读哪份时，按下面顺序：
> 先看「① 入门」跑通，再看「③ 结论汇总」了解现状，需要细节才翻「④ 专题」。
>
> 带 🕘 的是**历史归档**：数据仍有效、可复现，但结论已被更新的文档覆盖，不要直接引用其结论。

---

## ① 入门（第一次用）

| 文档 | 内容 |
|---|---|
| **[快速上手-三步测试Demo.md](快速上手-三步测试Demo.md)** | 选图片目录 → 框选模板 → 执行匹配，3 分钟跑通 GUI，不必读其它文档 |
| **[GeoGUIDemo使用说明.md](GeoGUIDemo使用说明.md)** | GUI 全部页签、参数含义、掩码涂抹、结果表、复现留痕 |

## ② API 手册

| 文档 | 内容 |
|---|---|
| **[SDK.md](SDK.md)** | 在自己的程序里调用库：链接配置、完整 API 表、参数调优、线程安全、FAQ、可编译示例。<br>其中 **5.6 位姿与几何（上位机对接）** 是给机器人/上位机用的：`poseMatrix` / `cornersAt` / `contourAt` / `setOrigin` 等 |

## ③ 结论汇总（现行，优先读这两份）

| 文档 | 内容 |
|---|---|
| **[与Halcon差距清单.md](与Halcon差距清单.md)** | **总索引**。能力缺口 / 性能 / 工程化三类差距，按 P0~P3 排优先级；含耗时拆解、硬件利用现状、OpenCL 结论、已被证伪的「差距」 |
| **[数据集四器综合对比-2026-11-16.md](数据集四器综合对比-2026-11-16.md)** | 四器（本项目 / Halcon 17 × Shape / NCC）在 `dataset` 上的检出率、Δscore、Δangle、Δpos、耗时，以及 67 个极端参数用例在 x64/x86 的鲁棒性矩阵（0 崩溃） |

## ④ 专题深入

| 文档 | 内容 | 备注 |
|---|---|---|
| [ShapeMatch-vs-Halcon.md](ShapeMatch-vs-Halcon.md) | 形状匹配 vs Halcon：667 张晶圆标定图的分数分布、位置一致性、掩码/评分模式等 11 组实验 | 第 10 节「positive-mean 是通用解」**已被推翻**（错位置虚高到 0.946，Halcon 0.556），开头有修订说明 |
| [NCC-vs-Halcon.md](NCC-vs-Halcon.md) | NCC vs Halcon：精度、速度拆解、旋转模板构建的优化过程与后续方案 | — |

## ⑤ 数据集说明

| 文档 | 内容 |
|---|---|
| [../dataset/sample/README.md](../dataset/sample/README.md) | 入库的三组抽样数据（data1 焊盘 6 张、data2 通孔 8 张、synth 合成 80 张）与判读口径 |
| [../dataset/image/README.md](../dataset/image/README.md) | Halcon 经典示例图（螺母/卡簧/齿轮/圆环/logo/qfn 大图）怎么跑 |
| [../HalconDemo/README.md](../HalconDemo/README.md) | Halcon 17 对照程序怎么编（本机只有 32 位 SDK） |

---

## 历史归档（🕘 数据有效，结论已被取代）

这些是分阶段排查的过程记录。**要复现具体数字时有用**，但选型/调参请以「③ 结论汇总」为准。

| 文档 | 内容 | 被谁取代 |
|---|---|---|
| 🕘 [data1-data2四器对比报告.md](data1-data2四器对比报告.md) | 2026-11-14，`dataset/data1`(117 张) / `data2`(36 张) 的四器对比 | ③ 数据集四器综合对比（那两份全量数据集本机已不存在，未入库） |
| 🕘 [抽样对比-2026-11-14.md](抽样对比-2026-11-14.md) | 2026-11-14，同模板同参数的抽样复测（含 `skipWeight` 修复后） | ③ 数据集四器综合对比 |
| 🕘 [test2四器对比-进度-2026-11-14.md](test2四器对比-进度-2026-11-14.md) | 交接文档：test2 抽样的对比进度 | ③ |
| 🕘 [data2-90度漏检复查-2026-11-14.md](data2-90度漏检复查-2026-11-14.md) | 交接文档：data2 −90° 簇漏检的排查与修复 | ③ / `与Halcon差距清单.md` |

---

## 图片

`docs/*.jpg` / `*.png` 是 README 引用的实测截图，均由 **CLI 画出来的**（不是 GUI 截图）：

```bat
build\GeoMatchDemo.exe -t case1\temp.jpg -d case1 -o _ai_tmp\docout -loops 5 -minscore 0.70 -max 12 -noshow
```

取 `out\model_info.png` 与结果图即可。`docs/img/` 是 GUI 分步截图，供 `快速上手-三步测试Demo.md` 使用。
