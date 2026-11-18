# 文档索引

> 根目录 [README.md](../README.md) 只有两件事：**这个库干什么** + **最小 API**。
> 其余内容都在这里。按需求查：

---

## ① 我要调参数

| 文档 | 内容 |
|---|---|
| **[参数手册.md](参数手册.md)** | **唯一一份讲参数数值语义的文档**：建模/搜索参数的默认值、取值范围、含义、"什么时候该改"；调参顺序；三个"层数"的区别；参数→CLI→GUI 对照表 |
| [GeoGUIDemo使用说明.md](GeoGUIDemo使用说明.md) | 界面操作 + **每个控件默认值/影响/场景**（按右栏 ①②③④⑤ 分组逐条讲） |
| [快速上手-三步测试Demo.md](快速上手-三步测试Demo.md) | 3 分钟跑通 GUI：选目录 → 框选模板 → 执行匹配 |

## ② 我要集成 SDK

| 文档 | 内容 |
|---|---|
| [SDK.md](SDK.md) | 链接配置、完整 API 表、参数调优、线程安全、FAQ、可编译示例。其中 **5.6 位姿与几何（上位机对接）** 讲 `poseMatrix` / `cornersAt` / `setOrigin` |

## ③ 我想知道它和 Halcon 差多少

| 文档 | 内容 |
|---|---|
| **[与Halcon差距清单.md](与Halcon差距清单.md)** | **总索引**：已对标/未对标的函数与参数、能力缺口、性能、工程化，按 P0~P3 排优先级；含耗时拆解、硬件利用现状、已被证伪的"差距" |
| [数据集四器综合对比-2026-11-16.md](数据集四器综合对比-2026-11-16.md) | 四器（本项目 / Halcon 17 × Shape / NCC）在 `dataset` 上的检出率、Δscore、Δangle、Δpos、耗时；67 个极端参数用例在 x64/x86 的鲁棒性矩阵（0 崩溃） |

## ④ 专题深入

| 文档 | 内容 | 备注 |
|---|---|---|
| [ShapeMatch-vs-Halcon.md](ShapeMatch-vs-Halcon.md) | 形状匹配 vs Halcon：667 张晶圆标定图的分数分布、位置一致性、掩码/评分模式等 11 组实验 | 第 10 节「positive-mean 是通用解」**已被推翻**，开头有修订说明 |
| [NCC-vs-Halcon.md](NCC-vs-Halcon.md) | NCC vs Halcon：精度、速度拆解、旋转模板构建的优化过程与后续方案 | — |

## ⑤ 数据集说明

| 文档 | 内容 |
|---|---|
| [../dataset/sample/README.md](../dataset/sample/README.md) | 入库的三组抽样数据（data1 焊盘 6 张、data2 通孔 8 张、synth 合成 80 张）与判读口径 |
| [../dataset/image/README.md](../dataset/image/README.md) | Halcon 经典示例图（螺母/卡簧/齿轮/圆环/logo/qfn 大图）怎么跑 |
| [../HalconDemo/README.md](../HalconDemo/README.md) | Halcon 17 对照程序怎么编（本机只有 32 位 SDK） |

---

## 历史归档（🕘 数据有效，结论已被取代）

过程记录，复现具体数字时有用；**选型/调参请以 ③ 为准**。

| 文档 | 内容 | 被谁取代 |
|---|---|---|
| 🕘 [data1-data2四器对比报告.md](data1-data2四器对比报告.md) | 2026-11-14，`dataset/data1`(117 张) / `data2`(36 张) 的四器对比 | ③ 数据集四器综合对比（那两份全量数据集本机已不存在） |
| 🕘 [抽样对比-2026-11-14.md](抽样对比-2026-11-14.md) | 2026-11-14，同模板同参数的抽样复测（含 `skipWeight` 修复后） | ③ |
| 🕘 [test2四器对比-进度-2026-11-14.md](test2四器对比-进度-2026-11-14.md) | 交接文档：test2 抽样的对比进度 | ③ |
| 🕘 [data2-90度漏检复查-2026-11-14.md](data2-90度漏检复查-2026-11-14.md) | 交接文档：data2 −90° 簇漏检的排查与修复 | ③ / `与Halcon差距清单.md` |

---

## 图片

CLI 画出来的实测图（`model_info.png`、`match_result*.jpg`、`synth_*.png` 等）与 GUI 截图
（`img/step*.png`、`gui_*.png/jpg`）都由脚本重生成：

```bat
build\GeoMatchDemo.exe -t case1\temp.jpg -d case1 -o _ai_tmp\docout -loops 5 -minscore 0.70 -max 12 -noshow
powershell -File tools\update-doc-images.ps1              :: 重拍 GUI 截图 + 重标注 + 自检
powershell -File tools\update-doc-images.ps1 -SkipCapture :: 只用已有原图重标注
```
