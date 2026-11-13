//***********************************************************************
//  example_shape_match.cpp - 形状匹配（梯度）最小可用示例
//
//  这是 SDK 文档 docs/SDK.md 里"快速开始"那一段的完整代码，
//  构建时会一起编译，保证文档里的代码永远是对的。
//
//  用法:
//      example_shape_match <模板图> <搜索图> [输出标注图]
//      example_shape_match case1/temp.jpg case1/tmp_grab_1762994178.jpg out.png
//
//  编译（MSVC，静态库）:
//      cl /std:c++17 /EHsc /MD /I<sdk>\include example_shape_match.cpp ^
//         <sdk>\lib\GeoMatch.lib <opencv>\opencv_world455.lib
//***********************************************************************
#include "GeoMatch/GeoMatch.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: %s <template> <image> [out.png]\n", argv[0]);
        std::printf("  e.g. %s case1/temp.jpg case1/tmp_grab_1762994178.jpg out.png\n", argv[0]);
        return 1;
    }

    //---- 1) 读图（灰度即可；传彩色图也行，库内部会转灰度） ----
    const cv::Mat tpl = cv::imread(argv[1], cv::IMREAD_GRAYSCALE);
    const cv::Mat img = cv::imread(argv[2], cv::IMREAD_GRAYSCALE);
    if (tpl.empty() || img.empty())
    {
        std::fprintf(stderr, "cannot read %s / %s\n", argv[1], argv[2]);
        return 2;
    }
    std::printf("GeoMatch %s (%s build)\n", geomatch::versionString(),
                geomatch::isSharedBuild() ? "dll" : "static");
    std::printf("template %dx%d   image %dx%d\n",
                tpl.cols, tpl.rows, img.cols, img.rows);

    //---- 2) 配置：建模参数 + 搜索参数放在同一个 Config 里 ----
    geomatch::Config cfg;
    cfg.angleStart  = -CV_PI;        // 角度范围；已知只有小角度就收窄，搜索更快
    cfg.angleExtent = 2 * CV_PI;
    cfg.scaleMin    = 1.0;           // 不需要尺度搜索就都填 1.0（最快）
    cfg.scaleMax    = 1.0;
    cfg.minContrast = 12.0;          // 灰度差单位；调大 = 只用强边，点会变少
    cfg.maxPoints   = 1200;
    cfg.numLevels   = 0;             // 0 = 自动。不要填 1，会退化成暴力搜索

    cfg.minScore    = 0.45;          // 默认评分是 mean（与 Halcon 一致）
    cfg.maxMatches  = 5;
    cfg.scoreMode   = 0;             // 0 = mean；不要用 1（分数会虚高）
    cfg.subpixel    = 1;             // 0 关 / 1 抛物线 / 2 最小二乘

    //---- 3) 建模型（或 m.load("model.shm") 复用已经建好的） ----
    geomatch::Matcher m;
    if (!m.create(tpl, cfg))
    {
        std::fprintf(stderr, "create model failed (try lowering minContrast)\n");
        return 3;
    }
    std::printf("model: %d points, %d levels, %dx%d, spread %.2f\n",
                m.modelPointCount(), m.numLevels(),
                m.modelSize().width, m.modelSize().height,
                m.pointSpreadRatio());

    //---- 4) 搜索 ----
    geomatch::Timing tm;
    const std::vector<geomatch::Detection> dets = m.detect(img, &tm);
    std::printf("matches: %d   preprocess %.2f ms   search %.2f ms\n",
                (int)dets.size(), tm.preprocessMs, tm.searchMs);

    for (size_t i = 0; i < dets.size(); i++)
    {
        const geomatch::Detection& d = dets[i];
        std::printf("  #%d  score %.3f  pos (%.2f, %.2f)  angle %.1f deg  scale %.3f\n",
                    (int)i, d.score, d.pos.x, d.pos.y,
                    d.angle * 180.0 / CV_PI, d.scale);
    }

    //---- 5) 画结果（可选） ----
    if (argc > 3 && !dets.empty())
    {
        cv::Mat vis = m.drawResult(img, dets);
        if (!vis.empty() && cv::imwrite(argv[3], vis))
            std::printf("annotated image -> %s\n", argv[3]);
    }
    return dets.empty() ? 4 : 0;
}
