# 样例数据（可直接跑测试）

仓库里的 `dataset/` 整包太大（data1 44 MB / data2 11 MB），所以只抽了一小份放在这里，
便于 clone 后立刻验证匹配效果、也便于写自动化脚本时有一份固定输入。

```
dataset/sample/
  data1/   template_pad.jpg（140×140 白色焊盘） + 6 张 1280×1024 实拍图
  data2/   template_via.jpg（110×110 通孔标记） + 8 张 1280×1024 实拍图
```

原始数据集是产线相机拍的 PCB 板图，文件名里的 `_score` 是采集端自己带的后缀，
与匹配分数无关。`case1/` 里那套（蛇形走线 U 弯）本来就已经入库，也可以一起用。

---

## data1：白色方形焊盘（模型点多、对比度高）

图上有多个焊盘，**默认会检出 12 个**，演示时建议用 `-max 1` 只看最强的一个。

```bat
build\GeoMatchDemo.exe -t dataset\sample\data1\template_pad.jpg ^
    -d dataset\sample\data1 -angle 20 -contrast 120 -hi 160 ^
    -minscore 0.5 -max 1 -noshow
```

- `-angle 20`：目标基本不转向（实测都在 ±1.2° 内），扫 20° 就够；
- `-contrast 120 -hi 160`：**这是关键**。焊盘边缘很锐利，用 CLI 默认的 12/30（= 界面上的
  灰度差 3/7.5）建出来的模型会混进大量弱边点，全点均值只有 ~0.65；提到 120/160 后
  模型点仍然是 1198 个，但分数直接到 0.97+（Halcon 同图约 0.956）。
  界面里对应的「最小对比度(灰度差)」是 30、高阈值 40。

实测（x64 Release，1.1.1，`-minscore 0.5 -max 1`）：**6/6 全部检出**

| 图像 | 得分 | 角度 | 位置 (x;y) |
|---|---|---|---|
| scan_1783729947577_score.jpg | 1.000 | 0.0° | (1110;154) |
| scan_1783730056171_score.jpg | 0.970 | -1.2° | (955;162) |
| scan_1783730062775_score.jpg | 0.972 | -0.1° | (954;155) |
| scan_1783730070463_score.jpg | 0.886 | 0.0° | (788;151) |
| scan_1783730078944_score.jpg | 0.968 | 0.0° | (948;156) |
| scan_1783730751849_score.jpg | 0.970 | -0.1° | (231;162) |

---

## data2：蛇形走线上的通孔标记（含大角度与外观差异）

同一批图里目标朝向分两簇：**0°** 和 **−90°**，所以角度范围要开 `-angle-start -100 -angle 200`。

```bat
build\GeoMatchDemo.exe -t dataset\sample\data2\template_via.jpg ^
    -d dataset\sample\data2 -angle-start -100 -angle 200 ^
    -minscore 0.4 -max 1 -noshow
```

实测（1.1.1，`-minscore 0.4 -max 1`）：**7/8 检出**

| 图像 | 得分 | 角度 | 说明 |
|---|---|---|---|
| scan_1776310359774_score.jpg | 0.995 | 0.1° | 0° 组 |
| scan_1776310360070_score.jpg | 0.927 | 0.0° | 0° 组 |
| scan_1776310360404_score.jpg | 0.915 | 0.1° | 0° 组 |
| scan_1776310360723_score.jpg | 0.905 | 0.1° | 0° 组 |
| scan_1776334904019_score.jpg | 0.437 | 4.5° | −90° 组（外观差异，见下） |
| scan_1776336136494_score.jpg | —— | —— | −90° 组，0.40 阈值下未检出 |
| scan_1776336639895_score.jpg | 0.442 | 8.3° | −90° 组 |
| scan_1776336640107_score.jpg | 0.439 | 7.2° | −90° 组 |

−90° 那几张拍到的实例与模板本身外观有差异（双暗孔 + 斑纹），分数天然低：
Halcon 17 在同一批图上也只有 0.5~0.6。要在这组追召回就把 `-minscore` 降到 0.40、
或把界面里的「弱梯度跳过」从 0 调到 0.9（分数会整体抬高 0.1~0.3，代价是重复纹理上
容易出现高分误检，详见 README 的「重复纹理高分重叠」一节）。

---

## 在界面里跑

1. 「1. 模板图像 → 选择模板图片...」选 `dataset/sample/data1/template_pad.jpg`；
2. 「2. 匹配目标 → 选择图片目录...」选 `dataset/sample/data1`；
3. 把「最小对比度(灰度差)」调到 **30**、高阈值 **40**（data1 必须，见上），角度范围 20°；
4. 「4. 执行匹配 → 执行匹配」。

data2 则把模板换成 `template_via.jpg`、目录换成 `data2`、角度范围改成 −100°~100°。

---

## 用库自己写程序

`examples/example_shape_match.cpp` 就是最小用法，直接传入这里的路径即可：

```bat
build\example_shape_match.exe dataset\sample\data1\template_pad.jpg ^
    dataset\sample\data1\scan_1783729947577_score.jpg out.png
```

## 许可

这些图来自本项目的实际测试采集，随仓库一起以仓库的 LICENSE 发布，仅用于功能验证与对比测试。
