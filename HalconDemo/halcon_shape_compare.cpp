//======================================================================
//  halcon_shape_compare.cpp - Halcon 17 形状匹配对比 Demo（32 位）
//
//  用途：用 Halcon 17 create_scaled_shape_model / find_scaled_shape_model
//  在同一模板、同一搜索图、同一角度/尺度范围下跑形状匹配，
//  输出与本项目 GeoMatch/ShapeMatch（GeoMatchDemo）格式对齐的结果，
//  方便评估两者在分数、位置、角度、尺度、速度上的实际差距。
//
//  注意：本机 Halcon 17 仅提供 32 位库（Halcon17/x86sse2-win32），
//  因此本 Demo 必须以 Win32（x86）构建，不能与主项目 x64 构建混链。
//  构建脚本：tools/build-halcon-compare.bat（用 vcvars32.bat 设 32 位环境）。
//
//  用法：
//    halcon_shape_compare <template> [search_image] [options]
//
//  示例：
//    halcon_shape_compare case2/template_a.jpg case2/search_a_03.jpg -minscore 0.3 -max 1
//    halcon_shape_compare case1/temp.jpg case1/tmp_grab_1762994178.jpg -minscore 0.7 -max 1
//
//  输出（与 GeoMatchDemo 对齐）：
//    model created  tpl=...  create=xx ms
//    <search_image>  matches=N  search=xx ms
//       #0  score=0.9xx  pos=(x;y)  angle=xx.xxdeg  scale=x.xxx
//======================================================================
#include "halconcpp/HalconCpp.h"    // Halcon C++ 总头文件（含 HShapeModel/HImage/HException 及导出宏）
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <chrono>

using namespace HalconCpp;

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

static double now_ms()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static const char* opt(const std::vector<std::string>& args,
                       const std::string& key, const char* def)
{
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return args[i + 1].c_str();
    return def;
}

static double optd(const std::vector<std::string>& args,
                   const std::string& key, double def)
{
    const char* v = opt(args, key, nullptr);
    return v ? std::atof(v) : def;
}

static int opti(const std::vector<std::string>& args,
                const std::string& key, int def)
{
    const char* v = opt(args, key, nullptr);
    return v ? std::atoi(v) : def;
}

static bool has(const std::vector<std::string>& args, const std::string& key)
{
    for (const auto& a : args)
        if (a == key) return true;
    return false;
}
int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty())
    {
        printf("usage: halcon_shape_compare <template> [search_image] [options]\n"
               "  options:\n"
               "    -angle-start <deg>   起始角度，默认 -180\n"
               "    -angle-extent <deg>  角度范围，默认 360\n"
               "    -smin <scale>        最小尺度，默认 1.0\n"
               "    -smax <scale>        最大尺度，默认 1.0\n"
               "    -minscore <score>    最小得分，默认 0.7\n"
               "    -max <N>             最多匹配数，默认 12\n"
               "    -max-overlap <v>     最大重叠率，默认 1.0\n"
               "    -subpixel <true|false> 亚像素精化，默认 true\n"
               "    -levels <N>          金字塔层数，0=自动，默认 0\n"
               "    -greediness <v>      贪婪因子，默认 0.9\n"
               "    -contrast <v>        高阈值，默认 30\n"
               "    -min-contrast <v>    低阈值，默认 12\n"
               "    -save-model <path>   将模型写入文件（跳过后续创建）\n"
               "    -load-model <path>   从文件加载模型（不重新创建）\n"
               "    -loop <N>            重复查找 N 次，取最快耗时\n"
               "  example: halcon_shape_compare case2/template_a.jpg case2/search_a_03.jpg -minscore 0.3 -max 1\n");
        return 1;
    }

    const char* tplPath = args[0].c_str();
    const char* searchPath = (args.size() > 1 && args[1][0] != '-')
                                 ? args[1].c_str()
                                 : nullptr;

    double angleStartDeg  = optd(args, "-angle-start", -180.0);
    double angleExtentDeg = optd(args, "-angle-extent", 360.0);
    double scaleMin       = optd(args, "-smin", 1.0);
    double scaleMax       = optd(args, "-smax", 1.0);
    double minScore       = optd(args, "-minscore", 0.7);
    Hlong   numMatches    = opti(args, "-max", 12);
    double maxOverlap     = optd(args, "-max-overlap", 1.0);
    const char* subpixel  = opt(args, "-subpixel", "true");
    Hlong   numLevels     = opti(args, "-levels", 0);
    double greediness     = optd(args, "-greediness", 0.9);
    Hlong   contrast      = (Hlong)optd(args, "-contrast", 30);
    Hlong   minContrast   = (Hlong)optd(args, "-min-contrast", 12);
    const char* saveModel = opt(args, "-save-model", nullptr);
    const char* loadModel = opt(args, "-load-model", nullptr);
    int     loop          = opti(args, "-loop", 1);
    try
    {
        //---- 读模板 ----------------------------------------------------
        HImage tpl;
        tpl.ReadImage(tplPath);

        //---- 创建模型（Halcon create_scaled_shape_model）-------------
        HShapeModel model;
        double t0 = now_ms();
        if (loadModel)
        {
            model.ReadShapeModel(loadModel);
        }
        else
        {
            // AngleStart / AngleExtent 必须是弧度；AngleStep=0 表示自动；
            // ScaleStep=0 表示自动；Optimization="none"；Metric="use_polarity"。
            model = HShapeModel(tpl,
                                numLevels,
                                angleStartDeg * M_PI / 180.0,   // AngleStart (rad)
                                angleExtentDeg * M_PI / 180.0,  // AngleExtent (rad)
                                0.0,                            // AngleStep (0=auto)
                                scaleMin, scaleMax, 0.0,        // ScaleMin/Max/Step
                                "none",                         // Optimization
                                "use_polarity",                 // Metric
                                contrast, minContrast);         // Contrast / MinContrast
        }
        double tCreate = now_ms() - t0;

        if (saveModel)
            model.WriteShapeModel(saveModel);

        // 搜索图列表（命令行给了一张就只跑一张；不给则只对模板自检不跑搜索）
        std::vector<std::string> searchImages;
        if (searchPath)
            searchImages.push_back(searchPath);

        printf("model created  tpl=%s  create=%.2f ms\n", tplPath, tCreate);
        printf("angle: %.1f..%.1f deg  scale: %.3f..%.3f  minscore=%.3f  maxmatches=%d  overlap=%.2f\n",
               angleStartDeg, angleStartDeg + angleExtentDeg,
               scaleMin, scaleMax, minScore, (int)numMatches, maxOverlap);
        printf("subpixel=%s  levels=%d  greediness=%.2f  contrast=%d  mincontrast=%d\n",
               subpixel, (int)numLevels, greediness,
               (int)contrast, (int)minContrast);

        //---- 对每张搜索图查找 ------------------------------------------
        for (const auto& imgPath : searchImages)
        {
            HImage img;
            img.ReadImage(imgPath.c_str());

            HTuple row, col, angle, scale, score;
            double bestSearch = 1e9;
            for (int i = 0; i < loop; ++i)
            {
                double ts = now_ms();
                model.FindScaledShapeModel(
                    img,
                    angleStartDeg * M_PI / 180.0, angleExtentDeg * M_PI / 180.0,
                    scaleMin, scaleMax, minScore, numMatches, maxOverlap,
                    subpixel, numLevels, greediness,
                    &row, &col, &angle, &scale, &score);
                double te = now_ms();
                double dt = te - ts;
                if (dt < bestSearch) bestSearch = dt;
            }

            Hlong n = row.Length();
            printf("\n%s  matches=%d  search=%.2f ms\n",
                   imgPath.c_str(), (int)n, bestSearch);
            for (Hlong i = 0; i < n; ++i)
            {
                double r = row[i].D();    // Halcon row = y
                double c = col[i].D();    // Halcon col = x
                double a = angle[i].D() * 180.0 / M_PI;
                double s = scale[i].D();
                double sc = score[i].D();
                printf("   #%d  score=%.4f  pos=(%.2f;%.2f)  angle=%.2fdeg  scale=%.4f\n",
                       (int)i, sc, c, r, a, s);
            }
        }
    }
    catch (HException& e)
    {
        printf("Halcon error: %s\n", e.ErrorMessage().Text());
        return 1;
    }
    catch (...)
    {
        printf("unknown error\n");
        return 1;
    }
    return 0;
}