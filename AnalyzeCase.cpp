//***********************************************************************
//  AnalyzeCase - 命令行: 拿一个已保存的模型去跑一批真实图片，把每张图的
//  识别细节(分数/位姿/耗时/图像质量)打出来，并给出统计。
//
//  用法:
//    AnalyzeCase <图片目录> <模型文件> [选项]
//
//  选项:
//    --min-score X   最小分数(默认 0.5)
//    --limit N       只跑前 N 张(默认全跑)
//    --dump <目录>   把带框的结果图写到目录里
//    --probe N       对前 N 张图做位姿探针: 看看在找到的位姿附近分数怎么变
//    --cpu           关掉 OpenCL，用 CPU 跑
//    --sweep         用目录里的 *Template* 图重建模型，扫一遍参数
//                    (在 --limit 张图上评估，默认 15 张)
//
//  例子:
//    AnalyzeCase build/2026-10-23/2026-10-23 build/2026-10-23/ScanPlaneTemplate.png.GeoModel
//    AnalyzeCase ... --limit 20 --dump build/2026-10-23/out
//    AnalyzeCase ... --sweep --limit 15
//***********************************************************************
#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/core/utils/filesystem.hpp>
#include "GeoMatch/GeoMatch.h"

namespace
{

//-----------------------------------------------------------------------
//  图像质量: 亮度/对比度 + 边缘强度(和模型创建时的阈值口径一致)
//-----------------------------------------------------------------------
struct ImgStat
{
    double mean = 0, std = 0;       // 灰度均值 / 标准差
    double gradMean = 0;            // 梯度幅值均值(Sobel 3x3, 和建模一致)
    double edgeRatio = 0;           // 梯度 >= minContrast 的像素占比
    double gradP90 = 0;             // 梯度幅值的 90% 分位
    double lapVar = 0;              // 拉普拉斯方差: 越小越模糊
    double hfRatio = 0;             // 半分辨率/原分辨率的梯度比: 越大越模糊
};

ImgStat imageStats(const cv::Mat& gray, double mc)
{
    cv::Mat gx, gy, mag;
    cv::Sobel(gray, gx, CV_32F, 1, 0, 3);
    cv::Sobel(gray, gy, CV_32F, 0, 1, 3);
    cv::magnitude(gx, gy, mag);

    ImgStat st;
    cv::Scalar m, s;
    cv::meanStdDev(gray, m, s);
    st.mean = m[0];
    st.std  = s[0];

    cv::Scalar gm, gs;
    cv::meanStdDev(mag, gm, gs);
    st.gradMean = gm[0];

    const int total = gray.rows * gray.cols;
    st.edgeRatio = cv::countNonZero(mag >= mc) / (double)total;

    cv::Mat flat = mag.reshape(1, 1);
    cv::Mat sorted;
    cv::sort(flat, sorted, cv::SORT_EVERY_ROW | cv::SORT_ASCENDING);
    st.gradP90 = sorted.at<float>(0, (int)(total * 0.90));

    // 清晰度: 拉普拉斯方差
    cv::Mat lap;
    cv::Laplacian(gray, lap, CV_32F, 3);
    cv::Scalar lm, ls;
    cv::meanStdDev(lap, lm, ls);
    st.lapVar = ls[0] * ls[0];

    // 清晰度: 降采样后还剩多少梯度 -> 相对高频量, 越大说明越锐利
    cv::Mat small;
    cv::pyrDown(gray, small);
    cv::Mat sx, sy, smag;
    cv::Sobel(small, sx, CV_32F, 1, 0, 3);
    cv::Sobel(small, sy, CV_32F, 0, 1, 3);
    cv::magnitude(sx, sy, smag);
    cv::Scalar sm, ss;
    cv::meanStdDev(smag, sm, ss);
    st.hfRatio = (gm[0] > 1e-6) ? (sm[0] / gm[0]) : 0.0;
    return st;
}

struct OneResult
{
    std::string name;
    cv::Size    size;
    int         count = 0;
    double      score = 0;      // 最高分
    double      score2 = 0;     // 第二名(看余量)
    cv::Point2f pos;
    double angleDeg = 0, scale = 1;
    double preMs = 0, searchMs = 0;
    ImgStat st;
};

std::string fmtSize(const cv::Size& s)
{
    std::ostringstream os;
    os << s.width << "x" << s.height;
    return os.str();
}

void printModelInfo(const geomatch::Matcher& m, const std::string& file)
{
    const geomatch::Config c = m.config();
    const cv::Size         ms = m.modelSize();

    printf("==================== 模型 ====================\n");
    printf("文件        : %s\n", file.c_str());
    printf("模板尺寸    : %s   中心 (%.1f, %.1f)\n",
           fmtSize(ms).c_str(), m.modelCenter().x, m.modelCenter().y);
    printf("点数        : %d     层数: %d\n", m.modelPointCount(), m.numLevels());
    printf("角度        : %.1f .. %.1f 度 (范围 %.1f)\n",
           c.angleStart * 180.0 / CV_PI,
           (c.angleStart + c.angleExtent) * 180.0 / CV_PI,
           c.angleExtent * 180.0 / CV_PI);
    printf("缩放        : %.3f .. %.3f\n", c.scaleMin, c.scaleMax);
    printf("建模板参数  : minContrast(lo)=%.0f  contrast(hi)=%.0f  maxPoints=%d  numLevels=%d\n",
           c.minContrast, c.contrast, c.maxPoints, c.numLevels);
    printf("            (Sobel |g| 单位; 灰度差 = 值 / 4; 两者相等 = 纯阈值)\n");
    printf("搜索参数    : minScore=%.2f  maxMatches=%d  minDist=%.0f  greedy=%.2f  subpixel=%d\n",
           c.minScore, c.maxMatches, c.minDistance, c.greediness, c.subpixel);
    printf("评分/过滤   : scoreMode=%d  minCoverage=%.2f\n", c.scoreMode, c.minCoverage);
    printf("==============================================\n\n");
}

//-----------------------------------------------------------------------
//  模型点分析: 点是不是都堆在少数几条强边上? 靠后的点有多弱?
//-----------------------------------------------------------------------
void printModelPoints(const geomatch::Matcher& m)
{
    const std::vector<ModelPoint>& pts = m.core().modelPoints(0);
    if (pts.empty()) return;

    // 8x8 一格, 看点的空间分布
    const cv::Size ms = m.modelSize();
    const int cx = (ms.width + 7) / 8, cy = (ms.height + 7) / 8;
    std::vector<int> cell((size_t)cx * cy, 0);
    for (const ModelPoint& p : pts)
    {
        const int bx = (int)((p.x + m.modelCenter().x) / 8);
        const int by = (int)((p.y + m.modelCenter().y) / 8);
        if (bx >= 0 && bx < cx && by >= 0 && by < cy) cell[(size_t)by * cx + bx]++;
    }
    int occupied = 0, maxCell = 0;
    for (int c : cell) { if (c > 0) occupied++; maxCell = std::max(maxCell, c); }

    // 分段看梯度强度(点是按幅值从大到小排的)
    auto meanMag = [&](size_t from, size_t to)
    {
        if (to > pts.size()) to = pts.size();
        if (to <= from) return 0.0;
        double s = 0;
        for (size_t i = from; i < to; i++) s += pts[i].mag;
        return s / (double)(to - from);
    };

    printf("\n==================== 模型点 ====================\n");
    printf("点数 %d, 模板 %dx%d, 8x8 网格 %dx%d\n",
           (int)pts.size(), ms.width, ms.height, cx, cy);
    printf("有点的格子: %d / %d (%.0f%%),  单格最多 %d 个点\n",
           occupied, cx * cy, 100.0 * occupied / (double)(cx * cy), maxCell);
    const size_t n = pts.size();
    printf("梯度幅值均值: 前 100 = %.1f, 101-300 = %.1f, 301-600 = %.1f, "
           "601-1200 = %.1f, 1201-末尾 = %.1f\n",
           meanMag(0, 100), meanMag(100, 300), meanMag(300, 600),
           meanMag(600, 1200), meanMag(1200, n));

    // 调参提示
    const int   cand = m.core().candidatePixels();
    const float wm = m.core().weakestMag(), sm = m.core().strongestMag();
    printf("过阈值的像素 %d 个, 实际取 %d 个 (最强 %.0f, 最弱 %.0f, 比值 %.2f)\n",
           cand, (int)pts.size(), sm, wm, sm > 1e-6f ? wm / sm : 0.f);
    if (cand > 0 && cand > (int)pts.size() * 2)
        printf("  ! minContrast 没起作用: 候选 %d 个远多于点数 %d 个, "
               "实际是 maxPoints 在截断, 调 minContrast 不会有变化\n", cand, (int)pts.size());
    // 实测: 这个数据集上 400 点时比值 0.59(平均 0.917), 2025 点时 0.39(平均 0.851)
    if (sm > 1e-6f && wm / sm < 0.50f)
        printf("  ! 点集尾部偏弱: 最弱的点只有最强点的 %.0f%%, 模糊/噪声会先打掉它们 -> "
               "减小 maxPoints, 直到该比值 > 0.5 (本例 400~600)\n",
               100.0 * wm / sm);
    printf("================================================\n");
}

//-----------------------------------------------------------------------
//  汇总
//-----------------------------------------------------------------------
void printSummary(const std::vector<OneResult>& rs, double minScore)
{
    if (rs.empty()) return;

    std::vector<double> sc;
    for (const OneResult& r : rs) if (r.count > 0) sc.push_back(r.score);

    printf("\n==================== 汇总 ====================\n");
    printf("图片        : %d 张, 有结果 %d 张\n", (int)rs.size(), (int)sc.size());
    if (!sc.empty())
    {
        std::vector<double> v = sc;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        const double mean = sum / v.size();
        double sq = 0;
        for (double x : v) sq += (x - mean) * (x - mean);
        printf("最高分      : min=%.3f  平均=%.3f  中位=%.3f  max=%.3f  std=%.3f\n",
               v.front(), mean, v[v.size() / 2], v.back(), std::sqrt(sq / v.size()));
    }

    // 分数分布
    const int bins = 10;
    std::vector<int> hist(bins, 0);
    for (double s : sc)
    {
        int b = (int)((s - 0.0) / 0.1);
        b = std::min(std::max(b, 0), bins - 1);
        hist[b]++;
    }
    printf("\n分数分布(最高分):\n");
    for (int b = bins - 1; b >= 0; b--)
    {
        printf("  %.1f-%.1f : %3d  ", b * 0.1, b * 0.1 + 0.1, hist[b]);
        for (int k = 0; k < hist[b]; k++) putchar('#');
        printf("\n");
    }

    int low = 0;
    for (const OneResult& r : rs) if (r.score < minScore) low++;
    printf("\n低于 %.2f 的图: %d / %d\n", minScore, low, (int)rs.size());

    // 最差的几张
    std::vector<OneResult> worst = rs;
    std::sort(worst.begin(), worst.end(),
              [](const OneResult& a, const OneResult& b) { return a.score < b.score; });
    printf("\n最差的 10 张:\n");
    for (int i = 0; i < (int)worst.size() && i < 10; i++)
    {
        const OneResult& r = worst[i];
        printf("  %-28s score=%6.3f  n=%d  角度=%7.2f  缩放=%.3f  边缘占比=%.3f  gradP90=%5.1f\n",
               r.name.c_str(), r.score, r.count, r.angleDeg, r.scale,
               r.st.edgeRatio, r.st.gradP90);
    }

    double tPre = 0, tSearch = 0;
    for (const OneResult& r : rs) { tPre += r.preMs; tSearch += r.searchMs; }
    printf("\n耗时        : 预处理 %.2f ms  搜索 %.2f ms  合计 %.2f ms (平均每张)\n",
           tPre / rs.size(), tSearch / rs.size(), (tPre + tSearch) / rs.size());
    printf("==============================================\n");
}

//-----------------------------------------------------------------------
//  位姿探针: 在找到的位姿附近看看分数怎么变
//  score 掉得快 => 位姿已经是最优点; 换个角度/缩放更高 => 网格太粗
//-----------------------------------------------------------------------
void probePose(geomatch::Matcher& m, const cv::Mat& gray, const OneResult& r)
{
    ShapeMatcher& core = m.core();
    const double a0 = r.angleDeg * CV_PI / 180.0;

    printf("  probe @ (%.1f, %.1f) a=%.2f s=%.3f\n",
           r.pos.x, r.pos.y, r.angleDeg, r.scale);

    auto p = [&](const char* tag, cv::Point2f pos, double aDeg, double sc)
    {
        const double v = core.probe(gray, pos, aDeg * CV_PI / 180.0, sc);
        printf("    %-14s %6.3f\n", tag, v);
    };

    p("原位置", r.pos, r.angleDeg, r.scale);
    p("+1px x", cv::Point2f(r.pos.x + 1, r.pos.y), r.angleDeg, r.scale);
    p("+2px x", cv::Point2f(r.pos.x + 2, r.pos.y), r.angleDeg, r.scale);
    p("+1px y", cv::Point2f(r.pos.x, r.pos.y + 1), r.angleDeg, r.scale);
    p("+2px y", cv::Point2f(r.pos.x, r.pos.y + 2), r.angleDeg, r.scale);
    p("角度 +0.5", r.pos, r.angleDeg + 0.5, r.scale);
    p("角度 +1.0", r.pos, r.angleDeg + 1.0, r.scale);
    p("角度 -1.0", r.pos, r.angleDeg - 1.0, r.scale);
    p("缩放 +0.01", r.pos, r.angleDeg, r.scale + 0.01);
    p("缩放 +0.02", r.pos, r.angleDeg, r.scale + 0.02);
    p("缩放 -0.02", r.pos, r.angleDeg, r.scale - 0.02);
    (void)a0;
}

//-----------------------------------------------------------------------
//  参数扫描: 用模板图重建模型，在 subset 上评估
//-----------------------------------------------------------------------
struct CaseResult
{
    std::string tag;
    int    points = 0, levels = 0;
    double meanScore = 0, minScore = 0, maxScore = 0;
    int    found = 0, total = 0;
    double ms = 0;
};

void runSweep(const cv::Mat& tplGray,
              const geomatch::Config& base,
              const std::vector<std::string>& files,
              const std::vector<cv::Mat>& imgs,
              double minScore)
{
    struct Variant
    {
        const char* name;
        std::function<void(geomatch::Config&)> apply;
    };

    std::vector<Variant> variants;
    const double baseC = base.minContrast;
    const int    baseP = base.maxPoints;

    // 1) 最小对比度
    for (double mc : { 6.0, 10.0, 16.0, 25.0, 40.0, 60.0 })
    {
        std::string nm = "minContrast=" + std::to_string((int)mc);
        variants.push_back({ strdup(nm.c_str()), [mc](geomatch::Config& c) { c.minContrast = mc; } });
    }
    // 2) 点数
    for (int mp : { 300, 600, 1200, 2000, 3000 })
    {
        std::string nm = "maxPoints=" + std::to_string(mp);
        variants.push_back({ strdup(nm.c_str()), [mp](geomatch::Config& c) { c.maxPoints = mp; } });
    }
    // 3) 金字塔层数
    for (int nl : { 0, 2, 3, 4, 5 })
    {
        std::string nm = "numLevels=" + std::to_string(nl);
        variants.push_back({ strdup(nm.c_str()), [nl](geomatch::Config& c) { c.numLevels = nl; } });
    }
    // 4) 缩放范围
    for (double sr : { 0.0, 0.02, 0.05, 0.10, 0.20 })
    {
        std::string nm = "scale=+-" + std::to_string((int)(sr * 100)) + "%";
        variants.push_back({ strdup(nm.c_str()), [sr](geomatch::Config& c)
                            { c.scaleMin = 1.0 - sr; c.scaleMax = 1.0 + sr; } });
    }
    // 4b) 角度范围(模型只建了 +-2.5 度, 看看真实旋转有没有超出去)
    for (double ar : { 2.5, 5.0, 10.0, 20.0 })
    {
        std::string nm = "angle=+-" + std::to_string((int)ar) + "deg";
        variants.push_back({ strdup(nm.c_str()), [ar](geomatch::Config& c)
                            { c.angleStart = -ar * CV_PI / 180.0; c.angleExtent = 2 * ar * CV_PI / 180.0; } });
    }
    // 5) 贪婪度
    for (double g : { 0.6, 0.75, 0.85, 0.9, 0.95 })
    {
        std::string nm = "greediness=" + std::to_string(g).substr(0, 4);
        variants.push_back({ strdup(nm.c_str()), [g](geomatch::Config& c) { c.greediness = g; } });
    }
    // 6) 对比度 + 点数组合
    for (double mc : { 16.0, 25.0, 40.0 })
        for (int mp : { 600, 1200, 2000 })
        {
            std::string nm = "mc=" + std::to_string((int)mc) + "/pts=" + std::to_string(mp);
            variants.push_back({ strdup(nm.c_str()), [mc, mp](geomatch::Config& c)
                                { c.minContrast = mc; c.maxPoints = mp; } });
        }

    printf("\n==================== 参数扫描 ====================\n");
    printf("模板 %dx%d, 基线: minContrast=%.0f maxPoints=%d, 评估 %d 张图\n\n",
           tplGray.cols, tplGray.rows, baseC, baseP, (int)imgs.size());
    printf("%-22s %6s %4s %8s %8s %8s %6s %8s\n",
           "变体", "点数", "层", "平均分", "最低", "最高", "命中", "ms/张");

    std::vector<CaseResult> out;
    for (const Variant& v : variants)
    {
        geomatch::Config c = base;
        c.minScore  = minScore;
        v.apply(c);

        geomatch::Matcher m;
        if (!m.create(tplGray, c))
        {
            printf("%-22s  (建模板失败)\n", v.name);
            continue;
        }

        CaseResult cr;
        cr.tag    = v.name;
        cr.points = m.modelPointCount();
        cr.levels = m.numLevels();

        double sum = 0, mn = 1e9, mx = -1e9, tsum = 0;
        int found = 0;
        for (size_t i = 0; i < imgs.size(); i++)
        {
            geomatch::Timing tm;
            std::vector<geomatch::Detection> d = m.detect(imgs[i], &tm);
            const double s = d.empty() ? 0.0 : d[0].score;
            sum += s; mn = std::min(mn, s); mx = std::max(mx, s);
            if (!d.empty()) found++;
            tsum += tm.recognitionMs();
        }
        cr.total     = (int)imgs.size();
        cr.found     = found;
        cr.meanScore = sum / imgs.size();
        cr.minScore  = mn;
        cr.maxScore  = mx;
        cr.ms        = tsum / imgs.size();

        printf("%-22s %6d %4d %8.3f %8.3f %8.3f %4d/%-3d %8.2f\n",
               cr.tag.c_str(), cr.points, cr.levels, cr.meanScore, cr.minScore,
               cr.maxScore, cr.found, cr.total, cr.ms);
        out.push_back(cr);
    }

    // 按平均分排序，给出最好的几个
    std::sort(out.begin(), out.end(),
              [](const CaseResult& a, const CaseResult& b) { return a.meanScore > b.meanScore; });
    printf("\n平均分最高的 8 个:\n");
    for (int i = 0; i < (int)out.size() && i < 8; i++)
        printf("  %-22s 平均 %.3f  最低 %.3f  点数 %d  层 %d\n",
               out[i].tag.c_str(), out[i].meanScore, out[i].minScore,
               out[i].points, out[i].levels);

    // 最好的 3 个变体: 逐图打一遍, 看清楚每张图的分数/缩放/角度
    printf("\n最优变体逐图细节:\n");
    for (int k = 0; k < (int)out.size() && k < 3; k++)
    {
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const CaseResult& c) { return c.tag == out[k].tag; });
        (void)it;
        printf("\n--- %s (平均 %.3f) ---\n", out[k].tag.c_str(), out[k].meanScore);
        printf("  %-28s %6s %7s %7s\n", "图片", "分数", "缩放", "角度");

        geomatch::Config c = base;
        c.minScore = minScore;
        for (const Variant& v : variants)
            if (std::string(v.name) == out[k].tag) v.apply(c);
        geomatch::Matcher m;
        if (!m.create(tplGray, c)) continue;
        for (size_t i = 0; i < imgs.size(); i++)
        {
            std::vector<geomatch::Detection> d = m.detect(imgs[i]);
            const size_t p = files[i].find_last_of("\\/");
            const std::string nm = (p == std::string::npos) ? files[i] : files[i].substr(p + 1);
            printf("  %-28s %6.3f %7.3f %7.2f\n", nm.c_str(),
                   d.empty() ? 0.0 : d[0].score,
                   d.empty() ? 0.0 : d[0].scale,
                   d.empty() ? 0.0 : d[0].angle * 180.0 / CV_PI);
        }
    }
    printf("\n==================================================\n");
    (void)files;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("用法: AnalyzeCase <图片目录> <模型文件> [--min-score X] [--limit N] "
               "[--dump 目录] [--probe N] [--cpu] [--sweep] [--mask 掩码图] [--min-dist X] [--score-mode N] "
               "[--min-cover X]\n");
        return 1;
    }

    std::string dir   = argv[1];
    std::string model = argv[2];
    double minScore   = 0.5;
    double minDist    = 0.0;     // 0 = 用模型默认值
    int    scoreMode  = 1;       // 1 = positive-mean(默认), 0 = mean, 2 = trimmed-mean, 3 = clipped-mean
    double minCover   = 0.0;     // >0: 丢掉覆盖率低于它的结果(0 = 关闭)
    int    limit      = 0;
    int    probeN     = 0;
    int    topN       = 0;
    int    points     = 0;      // >0: 用模板图按这个点数重建模型
    bool   useOCL     = true;
    bool   sweep      = false;
    std::string dumpDir;
    std::string maskFile;

    for (int i = 3; i < argc; i++)
    {
        const std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if      (a == "--min-score") minScore = atof(next().c_str());
        else if (a == "--min-dist")  minDist  = atof(next().c_str());
        else if (a == "--score-mode") scoreMode = atoi(next().c_str());
        else if (a == "--min-cover") minCover  = atof(next().c_str());
        else if (a == "--limit")     limit    = atoi(next().c_str());
        else if (a == "--probe")     probeN   = atoi(next().c_str());
        else if (a == "--dump")      dumpDir  = next();
        else if (a == "--cpu")       useOCL   = false;
        else if (a == "--top")       topN     = atoi(next().c_str());
        else if (a == "--points")    points   = atoi(next().c_str());
        else if (a == "--mask")      maskFile = next();
        else if (a == "--sweep")     sweep    = true;
    }

    geomatch::enableOpenCL(useOCL);

    //---------------- 模型 ----------------
    geomatch::Matcher m;
    if (!m.load(model))
    {
        fprintf(stderr, "加载模型失败: %s\n", model.c_str());
        return 2;
    }

    geomatch::Config cfg   = m.config();
    cfg.minScore  = minScore;
    cfg.useOpenCL = useOCL;
    if (minDist > 0) cfg.minDistance = minDist;
    cfg.scoreMode = scoreMode;
    cfg.minCoverage = (float)minCover;      // 0 = 关闭; 见 FindParams::minCoverage

    // >0: 丢掉文件里的模型, 用模板图按指定点数重建
    if (points > 0)
    {
        std::vector<std::string> all = geomatch::listImages(dir, { ".png" });
        cv::Mat tpl;
        for (const std::string& f : all)
            if (f.find("Template") != std::string::npos)
            {
                tpl = cv::imread(f, cv::IMREAD_GRAYSCALE);
                if (!tpl.empty()) { printf("重建模型, 模板: %s (%dx%d)\n", f.c_str(), tpl.cols, tpl.rows); break; }
            }
        if (tpl.empty()) { fprintf(stderr, "没找到模板图\n"); return 2; }
        cfg.maxPoints = points;
        if (!maskFile.empty())
        {
            cfg.mask = cv::imread(maskFile, cv::IMREAD_GRAYSCALE);
            if (cfg.mask.empty()) { fprintf(stderr, "掩码读不出来: %s\n", maskFile.c_str()); return 2; }
        }
        if (!m.create(tpl, cfg)) { fprintf(stderr, "建模板失败\n"); return 2; }
    }

    m.setConfig(cfg);
    // 打印必须在 setConfig 之后，否则看到的是模型自带的默认搜索参数
    printModelInfo(m, model);
    if (points > 0)
        printf("重建模型: maxPoints=%d\n", points);
    printModelPoints(m);

    //---------------- 图片列表 ----------------
    std::vector<std::string> files = geomatch::listImages(dir, { ".jpg", ".jpeg", ".png", ".bmp" });
    // 模板图本身不参与评估
    for (size_t i = 0; i < files.size();)
    {
        std::string n = files[i];
        const size_t p = n.find_last_of("\\/");
        if (p != std::string::npos) n = n.substr(p + 1);
        if (n.find("Template") != std::string::npos) files.erase(files.begin() + i);
        else i++;
    }
    if (limit > 0 && (int)files.size() > limit) files.resize(limit);
    if (files.empty())
    {
        fprintf(stderr, "目录里没有图片: %s\n", dir.c_str());
        return 2;
    }

    //---------------- 扫描模式: 先把要评估的图读进来 ----------------
    if (sweep)
    {
        std::vector<cv::Mat> imgs;
        const int n = std::min((int)files.size(), limit > 0 ? limit : 15);
        for (int i = 0; i < n; i++)
        {
            cv::Mat g = cv::imread(files[i], cv::IMREAD_GRAYSCALE);
            if (!g.empty()) imgs.push_back(g);
        }
        // 找模板图
        std::vector<std::string> all = geomatch::listImages(dir, { ".png" });
        cv::Mat tpl;
        for (const std::string& f : all)
            if (f.find("Template") != std::string::npos)
            {
                tpl = cv::imread(f, cv::IMREAD_GRAYSCALE);
                if (!tpl.empty()) { printf("模板图: %s (%dx%d)\n\n", f.c_str(), tpl.cols, tpl.rows); break; }
            }
        if (tpl.empty())
        {
            fprintf(stderr, "没找到模板图(*Template*.png)\n");
            return 2;
        }
        runSweep(tpl, cfg, files, imgs, minScore);
        return 0;
    }

    //---------------- 逐图评估 ----------------
    printf("%-28s %9s %6s %14s %7s %6s %3s %6s %8s %6s\n",
           "图片", "尺寸", "分数", "位置(x,y)", "角度", "缩放", "n", "边缘", "lapVar", "HF");

    std::vector<OneResult> results;
    for (size_t i = 0; i < files.size(); i++)
    {
        const std::string path = files[i];
        std::string name = path;
        const size_t p = name.find_last_of("\\/");
        if (p != std::string::npos) name = name.substr(p + 1);

        cv::Mat gray = cv::imread(path, cv::IMREAD_GRAYSCALE);
        if (gray.empty())
        {
            printf("%-30s  (读不出来)\n", name.c_str());
            continue;
        }

        geomatch::Timing tm;
        const std::vector<geomatch::Detection> dets = m.detect(gray, &tm);

        OneResult r;
        r.name     = name;
        r.size     = gray.size();
        r.count    = (int)dets.size();
        r.preMs    = tm.preprocessMs;
        r.searchMs = tm.searchMs;
        r.st       = imageStats(gray, cfg.minContrast);
        if (!dets.empty())
        {
            r.score   = dets[0].score;
            r.pos     = dets[0].pos;
            r.angleDeg = dets[0].angle * 180.0 / CV_PI;
            r.scale   = dets[0].scale;
            if (dets.size() > 1) r.score2 = dets[1].score;
        }

        printf("%-28s %9s %6.3f %7.1f,%7.1f %7.2f %6.3f %3d %6.3f %8.1f %6.3f\n",
               name.c_str(), fmtSize(r.size).c_str(), r.score, r.pos.x, r.pos.y,
               r.angleDeg, r.scale, r.count, r.st.edgeRatio, r.st.lapVar, r.st.hfRatio);
        fflush(stdout);

        if (topN > 0)
        {
            for (int k = 0; k < (int)dets.size() && k < topN; k++)
                printf("      #%d score=%.3f  pos=(%.1f, %.1f)  angle=%.2f  scale=%.3f\n",
                       k, dets[k].score, dets[k].pos.x, dets[k].pos.y,
                       dets[k].angle * 180.0 / CV_PI, dets[k].scale);
        }

        if (probeN > 0 && (int)i < probeN && !dets.empty())
            probePose(m, gray, r);

        if (!dumpDir.empty() && !dets.empty())
        {
            cv::utils::fs::createDirectory(dumpDir);
            cv::Mat vis = m.drawResult(gray, dets);
            const std::string out = dumpDir + "/" + name;
            cv::imwrite(out, vis);
        }
        results.push_back(r);
    }

    printSummary(results, minScore);
    return 0;
}
