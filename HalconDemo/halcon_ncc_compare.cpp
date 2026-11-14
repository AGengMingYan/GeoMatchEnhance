//======================================================================
//  halcon_ncc_compare.cpp - Halcon 17 NCC matching compare demo (32-bit)
//
//  Runs create_ncc_model / find_ncc_model with the same template, image and
//  angle range as this project's NccDemo, printing score/position/angle/time
//  in a matching layout so the two NCC implementations can be compared.
//
//  Halcon 17 here is 32-bit only (Halcon17/x86sse2-win32); build with the
//  x86 toolchain via tools/build-halcon-ncc-compare.bat.
//
//  usage:
//    halcon_ncc_compare <template> [search_image] [options]
//      -angle-start <deg>   default -180
//      -angle-extent <deg>  default 360
//      -minscore <score>    default 0.7
//      -max <N>             default 1
//      -max-overlap <v>     default 0.5
//      -subpixel <s>        default "true"   (Halcon only accepts true/false)
//      -levels <N>          default 0 (auto)
//      -metric <s>          default "use_polarity"
//      -save-model <path>  write_ncc_model
//      -load-model <path>  read_ncc_model
//      -loop <N>            repeat find N times, report the fastest time
//
//  note: NCC has NO scale dimension in either Halcon or this project.
//======================================================================
#include "halconcpp/HalconCpp.h"
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
int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty())
    {
        printf("usage: halcon_ncc_compare <template> [search_image] [options]\n"
               "  -angle-start/-angle-extent deg, -minscore F, -max N,\n"
               "  -max-overlap F, -subpixel true|false, -levels N,\n"
               "  -metric use_polarity|ignore_global_polarity,\n"
               "  -save-model/-load-model path, -loop N\n");
        return 1;
    }

    const char* tplPath = args[0].c_str();
    const char* searchPath = (args.size() > 1 && args[1][0] != '-')
                                 ? args[1].c_str() : nullptr;

    double angleStartDeg  = optd(args, "-angle-start", -180.0);
    double angleExtentDeg = optd(args, "-angle-extent", 360.0);
    double minScore       = optd(args, "-minscore", 0.7);
    Hlong   numMatches    = opti(args, "-max", 1);
    double maxOverlap     = optd(args, "-max-overlap", 0.5);
    const char* subpixel  = opt(args, "-subpixel", "true");
    Hlong   numLevels     = opti(args, "-levels", 0);
    const char* metric    = opt(args, "-metric", "use_polarity");
    const char* saveModel = opt(args, "-save-model", nullptr);
    const char* loadModel = opt(args, "-load-model", nullptr);
    int     loop          = opti(args, "-loop", 1);

    try
    {
        HImage tpl;
        tpl.ReadImage(tplPath);
        if (tpl.CountChannels().I() != 1)
            tpl = tpl.Rgb1ToGray();

        HNCCModel model;
        double t0 = now_ms();
        if (loadModel)
        {
            model.ReadNccModel(loadModel);
        }
        else
        {
            // create_ncc_model: angles in radians, AngleStep 0 = auto.
            model = HNCCModel(tpl, numLevels,
                              angleStartDeg * M_PI / 180.0,
                              angleExtentDeg * M_PI / 180.0,
                              0.0, metric);
        }
        double tCreate = now_ms() - t0;
        if (saveModel) model.WriteNccModel(saveModel);

        printf("ncc model created  tpl=%s  create=%.2f ms\n", tplPath, tCreate);
        printf("angle: %.1f..%.1f deg  minscore=%.3f  maxmatches=%d  overlap=%.2f\n",
               angleStartDeg, angleStartDeg + angleExtentDeg,
               minScore, (int)numMatches, maxOverlap);
        printf("subpixel=%s  levels=%d  metric=%s\n",
               subpixel, (int)numLevels, metric);

        if (!searchPath) return 0;

        HImage img;
        img.ReadImage(searchPath);
        if (img.CountChannels().I() != 1)
            img = img.Rgb1ToGray();

        HTuple row, col, angle, score;
        double bestSearch = 1e9;
        for (int i = 0; i < loop; ++i)
        {
            double ts = now_ms();
            model.FindNccModel(img,
                               angleStartDeg * M_PI / 180.0,
                               angleExtentDeg * M_PI / 180.0,
                               minScore, numMatches, maxOverlap,
                               subpixel, numLevels,
                               &row, &col, &angle, &score);
            double dt = now_ms() - ts;
            if (dt < bestSearch) bestSearch = dt;
        }

        Hlong n = row.Length();
        printf("\n%s  matches=%d  search=%.2f ms\n",
               searchPath, (int)n, bestSearch);
        for (Hlong i = 0; i < n; ++i)
        {
            printf("   #%d  score=%.4f  pos=(%.2f;%.2f)  angle=%.2fdeg\n",
                   (int)i, score[i].D(), col[i].D(), row[i].D(),
                   angle[i].D() * 180.0 / M_PI);
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