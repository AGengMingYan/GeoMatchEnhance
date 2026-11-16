//***********************************************************************
//  example_host_integration.cpp - 把匹配结果交给上位机 / 机器人的完整示例
//
//  演示 docs/SDK.md「5.6 位姿与几何（上位机对接）」里的每个接口：
//      poseMatrix / poseAffine / contourAt / cornersAt / boundsAt
//      templateToImage / imageToTemplate / setOrigin / origin
//
//  用法:
//      example_host_integration <模板图> <搜索图> [输出标注图]
//      example_host_integration dataset/sample/data1/template_pad.jpg ^
//                               dataset/sample/data1/scan_1783730078944_score.jpg out.png
//
//  输出的四角点 / 位姿矩阵就是上位机（C#/Python 走 DLL，或 PLC 通信）真正
//  需要的东西：模板图像素坐标 -> 搜索图像像素坐标 的一个相似变换。
//***********************************************************************
#include "GeoMatch/GeoMatch.h"

#include <cstdio>
#include <string>
#include <vector>

static void printPose(const geomatch::Matcher& m, const geomatch::Detection& d, int idx)
{
    // ---- 1) 3x3 齐次矩阵：模板坐标 -> 图像坐标（可直接给 PLC / 上位机）
    const cv::Matx33d M = m.poseMatrix(d);
    std::printf("  #%d  score %.3f  模板->图像 3x3:\n", idx, d.score);
    std::printf("        [ %8.4f  %8.4f  %9.3f ]\n", M(0, 0), M(0, 1), M(0, 2));
    std::printf("        [ %8.4f  %8.4f  %9.3f ]\n", M(1, 0), M(1, 1), M(1, 2));
    std::printf("        [ %8.4f  %8.4f  %9.3f ]\n", M(2, 0), M(2, 1), M(2, 2));

    // 只有旋转+平移时，从矩阵反解出上位机常用的三个数
    const double tx   = M(0, 2), ty = M(1, 2);
    const double deg  = std::atan2(M(0, 1), M(0, 0)) * 180.0 / CV_PI;   // 图像 y 向下
    const double scal = std::sqrt(M(0, 0) * M(0, 0) + M(1, 0) * M(1, 0));
    std::printf("        => 平移 (%.3f, %.3f)  旋转 %.2f deg  尺度 %.4f\n",
                tx, ty, deg, scal);

    // ---- 2) 模板四角在图像中的位置：画框、算多边形、给机器人抓取位姿
    const std::vector<cv::Point2f> q = m.cornersAt(d);
    if (q.size() == 4)
    {
        std::printf("        四角(模板 TL,TR,BR,BL) ->");
        for (int i = 0; i < 4; i++) std::printf(" (%.1f,%.1f)", q[i].x, q[i].y);
        std::printf("\n");
        const cv::Rect2f bb = m.boundsAt(d);
        std::printf("        正包围盒: x=%.1f y=%.1f w=%.1f h=%.1f\n",
                    bb.x, bb.y, bb.width, bb.height);
    }

    // ---- 3) 参考点换算：模板上的任意一点落在图像的哪里
    const cv::Point2f tplPt(0.f, 0.f);                     // 模板左上角
    const cv::Point2f imgPt = m.templateToImage(d, tplPt);
    const cv::Point2f back  = m.imageToTemplate(d, imgPt); // 逆变换，应回到 tplPt
    std::printf("        模板(%.1f,%.1f) -> 图像(%.2f,%.2f) -> 回到模板(%.3f,%.3f)\n",
                tplPt.x, tplPt.y, imgPt.x, imgPt.y, back.x, back.y);

    // ---- 4) 轮廓：真正的边缘折线（不是散点），可直接画或做测量
    const std::vector<std::vector<cv::Point2f>> cont = m.contourAt(d);
    size_t nPts = 0;
    for (size_t i = 0; i < cont.size(); i++) nPts += cont[i].size();
    std::printf("        轮廓: %d 条折线 / %d 点\n", (int)cont.size(), (int)nPts);
}

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

    geomatch::Config cfg;
    cfg.angleStart  = -10.0 * CV_PI / 180.0;   // 已知只在小角度内旋转就收窄，更快
    cfg.angleExtent = 20.0 * CV_PI / 180.0;
    cfg.contrast    = 120.0;                   // 锐边目标：= 灰度差 30
    cfg.minContrast = 30.0;                    //             = 灰度差 7.5
    cfg.minScore    = 0.7;
    cfg.maxMatches  = 4;

    geomatch::Matcher m;
    if (!m.create(tpl, cfg))
    {
        std::fprintf(stderr, "create model failed\n");
        return 3;
    }

    //---- 参考点（可选）：把结果点从"模板中心"挪到"装配基准点" ----------
    // 假设基准点在模板左上角往内 (20, 40) 像素处。设完之后
    // Detection::pos 直接就是该点在图像里的位置，上位机不用再换算。
    m.setOrigin(cv::Point2f(20.f, 40.f));
    std::printf("origin (template coords) = (%.1f, %.1f)\n",
                m.origin().x, m.origin().y);

    const std::vector<geomatch::Detection> dets = m.detect(img);
    std::printf("matches = %d\n", (int)dets.size());

    for (size_t i = 0; i < dets.size(); i++)
        printPose(m, dets[i], (int)i);

    //---- 画出来：drawResult 会自动用参考点画中心标记，框仍贴着模板 ----
    if (argc > 3 && !dets.empty())
    {
        cv::Mat vis = m.drawResult(img, dets);
        // 用 contourAt() 自己再画一遍轮廓，证明它和库画的是同一条线
        for (size_t i = 0; i < dets.size(); i++)
        {
            const std::vector<std::vector<cv::Point2f>> cont = m.contourAt(dets[i]);
            for (size_t c = 0; c < cont.size(); c++)
            {
                std::vector<cv::Point> poly(cont[c].begin(), cont[c].end());
                if (poly.size() >= 2) cv::polylines(vis, poly, false, cv::Scalar(255, 0, 255), 1);
            }
        }
        if (cv::imwrite(argv[3], vis)) std::printf("annotated image -> %s\n", argv[3]);
    }
    return dets.empty() ? 4 : 0;
}
