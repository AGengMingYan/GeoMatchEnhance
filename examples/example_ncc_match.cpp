//***********************************************************************
//  example_ncc_match.cpp - NCC（灰度归一化互相关）最小可用示例
//
//  这是 SDK 文档 docs/SDK.md 里 NCC 那一段的完整代码，构建时会一起编译。
//
//  用法:
//      example_ncc_match <模板图> <搜索图> [输出标注图]
//      example_ncc_match case1/temp.jpg case1/tmp_grab_1762994178.jpg out.png
//
//  和形状匹配的区别：
//    * 比的是灰度值而不是梯度方向 -> 低对比度/模糊/纯纹理更稳
//    * 不搜尺度（和 Halcon 的 NCC 一致），没有 scale 输出
//    * 重复纹理场景会处处命中，这种情况改用形状匹配
//***********************************************************************
#include "NccMatch/NccMatch.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: %s <template> <image> [out.png]\n", argv[0]);
        return 1;
    }

    const cv::Mat tpl = cv::imread(argv[1], cv::IMREAD_GRAYSCALE);
    const cv::Mat img = cv::imread(argv[2], cv::IMREAD_GRAYSCALE);
    if (tpl.empty() || img.empty())
    {
        std::fprintf(stderr, "cannot read %s / %s\n", argv[1], argv[2]);
        return 2;
    }
    std::printf("template %dx%d   image %dx%d\n",
                tpl.cols, tpl.rows, img.cols, img.rows);

    //---- 1) 建模（对标 Halcon create_ncc_model） ----
    geomatch::NccParams p;
    p.angleStart  = -CV_PI;
    p.angleExtent = 2 * CV_PI;
    p.angleStep   = 0.0;        // 0 = 自动（1 度）
    p.numLevels   = 0;          // 0 = 自动
    p.metric      = 0;          // 0 = use_polarity, 1 = ignore_global_polarity
    // 模板里有不想参与相关的区域（背景、重复纹理）就涂个掩码：
    // p.mask = cv::imread("mask.png", cv::IMREAD_GRAYSCALE);   // 非零 = 参与

    geomatch::NccMatcher m;
    if (!m.createNccModel(tpl, p))
    {
        std::fprintf(stderr, "create_ncc_model failed\n");
        return 3;
    }
    std::printf("model: %dx%d  pixels %d  levels %d  angles %d  step %.2f deg\n",
                m.modelSize().width, m.modelSize().height, m.modelPixelCount(),
                m.numLevels(), m.angleCount(), m.angleStep() * 180.0 / CV_PI);

    //---- 2) 搜索（对标 Halcon find_ncc_model） ----
    geomatch::NccFindParams fp;
    fp.minScore   = 0.5;        // NCC 分数范围 [-1,1]，0.5 左右算"像"
    fp.maxMatches = 5;
    fp.maxOverlap = 0.5;        // Halcon 的 MaxOverlap
    fp.subpixel   = 1;

    const std::vector<geomatch::NccResult> res = m.findNccModel(img, fp);
    std::printf("matches: %d   preprocess %.2f ms   search %.2f ms\n",
                (int)res.size(), m.lastPreprocessTime(), m.lastSearchTime());

    for (size_t i = 0; i < res.size(); i++)
    {
        std::printf("  #%d  score %.3f  pos (%.2f, %.2f)  angle %.1f deg\n",
                    (int)i, res[i].score, res[i].pos.x, res[i].pos.y,
                    res[i].angle * 180.0 / CV_PI);
    }

    //---- 3) 画结果（可选） ----
    if (argc > 3 && !res.empty())
    {
        cv::Mat vis = m.drawResult(img, res);
        if (!vis.empty() && cv::imwrite(argv[3], vis))
            std::printf("annotated image -> %s\n", argv[3]);
    }
    return res.empty() ? 4 : 0;
}
