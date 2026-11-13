//***********************************************************************
//  NccDemo - command line front end for the NCC matcher.
//
//  The switches mirror Halcon's create_ncc_model / find_ncc_model so that a
//  recipe can be translated one to one:
//
//    create:  NccDemo -t1 -t tpl.png -save model.ncm
//                 -levels 0        (0 = auto)          num_levels
//                 -angle 360       (degrees, total)   angle_extent
//                 -angle-start -180                    angle_start
//                 -angle-step 1    (degrees, 0=auto)  angle_step
//                 -metric 0        (0 polarity, 1 ignore_global_polarity)
//                 -mask mask.png
//
//    search:  NccDemo -t2 -load model.ncm -s img.png -o out
//                 -minscore 0.5    min_score
//                 -max 1           num_matches
//                 -overlap 0.5     max_overlap
//                 -subpix 1        sub_pixel
//                 -levels 0        num_levels (search)
//
//    batch:   NccDemo -load model.ncm -d dir -o outdir
//
//  There is no -scale switch on purpose: Halcon's NCC is rotation only.
//***********************************************************************
#include "NccMatch/NccMatch.h"
#include "GeoMatch/GeoMatch.h"

#include <opencv2/core/utils/filesystem.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using geomatch::NccFindParams;
using geomatch::NccMatcher;
using geomatch::NccParams;
using geomatch::NccResult;

namespace
{
    bool has(const std::vector<std::string>& a, const std::string& k)
    {
        for (size_t i = 0; i < a.size(); i++) if (a[i] == k) return true;
        return false;
    }

    std::string arg(const std::vector<std::string>& a, const std::string& k,
                    const std::string& def)
    {
        for (size_t i = 0; i + 1 < a.size(); i++) if (a[i] == k) return a[i + 1];
        return def;
    }

    double argD(const std::vector<std::string>& a, const std::string& k, double def)
    {
        const std::string v = arg(a, k, "");
        return v.empty() ? def : atof(v.c_str());
    }

    void printUsage()
    {
        std::printf(
            "NccDemo - Halcon-like NCC (normalised cross correlation) matcher\n"
            "\n"
            "build a model:\n"
            "  NccDemo -t1 -t <template> -save <model.ncm> [options]\n"
            "     -levels N       pyramid levels, 0 = auto            (num_levels)\n"
            "     -angle DEG      total angle extent, default 360     (angle_extent)\n"
            "     -angle-start DEG                                    (angle_start)\n"
            "     -angle-step DEG step, 0 = 1 degree at level 0       (angle_step)\n"
            "     -metric 0|1     0 use_polarity, 1 ignore_global_polarity\n"
            "     -mask <png>     only these template pixels are used\n"
            "\n"
            "search one image:\n"
            "  NccDemo -t2 -load <model.ncm> -s <image> -o <outdir> [options]\n"
            "     -minscore F     default 0.5                         (min_score)\n"
            "     -max N          default 1                           (num_matches)\n"
            "     -overlap F      default 0.5                         (max_overlap)\n"
            "     -subpix 0|1     default 1                           (sub_pixel)\n"
            "     -levels N       search from this level, 0 = all     (num_levels)\n"
            "\n"
            "search a whole directory:\n"
            "  NccDemo -load <model.ncm> -d <dir> -o <outdir> [same options]\n"
            "\n"
            "  no -scale option: Halcon's NCC does not search scale.\n");
    }
}

//=======================================================================
//  main
//=======================================================================
int main(int argc, char** argv)
{
    std::vector<std::string> a;
    for (int i = 1; i < argc; i++) a.push_back(argv[i]);

    if (a.empty() || has(a, "-h") || has(a, "--help"))
    {
        printUsage();
        return a.empty() ? 1 : 0;
    }

    //---------------- create_ncc_model ----------------
    if (has(a, "-t1"))
    {
        const std::string tplFile   = arg(a, "-t", "case1/temp.jpg");
        const std::string modelFile = arg(a, "-save", "case1/model.ncm");

        cv::Mat tpl = cv::imread(tplFile, cv::IMREAD_GRAYSCALE);
        if (tpl.empty())
        {
            std::fprintf(stderr, "[ncc] cannot open template: %s\n", tplFile.c_str());
            return -1;
        }

        NccParams p;
        p.numLevels   = (int)argD(a, "-levels", 0);
        p.angleStart  = argD(a, "-angle-start", -180.0) * CV_PI / 180.0;
        p.angleExtent = argD(a, "-angle", 360.0) * CV_PI / 180.0;
        p.angleStep   = argD(a, "-angle-step", 0.0) * CV_PI / 180.0;
        p.metric      = (int)argD(a, "-metric", 0);

        const std::string maskFile = arg(a, "-mask", "");
        if (!maskFile.empty())
        {
            p.mask = cv::imread(maskFile, cv::IMREAD_GRAYSCALE);
            if (p.mask.empty())
            {
                std::fprintf(stderr, "[ncc] cannot open mask: %s\n", maskFile.c_str());
                return -1;
            }
        }

        NccMatcher m;
        const int64 t0 = cv::getTickCount();
        if (!m.createNccModel(tpl, p))
        {
            std::fprintf(stderr, "[ncc] create_ncc_model failed\n");
            return -1;
        }
        const double ms = (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();

        if (!m.writeNccModel(modelFile))
        {
            std::fprintf(stderr, "[ncc] cannot write model: %s\n", modelFile.c_str());
            return -1;
        }

        std::printf("[ncc] template : %s  (%dx%d)\n", tplFile.c_str(), tpl.cols, tpl.rows);
        std::printf("[ncc] model    : %s\n", modelFile.c_str());
        std::printf("[ncc] pixels   : %d\n", m.modelPixelCount());
        std::printf("[ncc] levels   : %d\n", m.numLevels());
        std::printf("[ncc] angle    : %.1f .. %.1f deg  step %.2f deg  (%d poses at level 0)\n",
                    p.angleStart * 180.0 / CV_PI,
                    (p.angleStart + p.angleExtent) * 180.0 / CV_PI,
                    m.angleStep() * 180.0 / CV_PI, m.angleCount());
        std::printf("[ncc] metric   : %d (%s)\n", p.metric,
                    p.metric == 1 ? "ignore_global_polarity" : "use_polarity");
        std::printf("[ncc] build    : %.1f ms\n", ms);
        return 0;
    }

    //---------------- find_ncc_model ----------------
    const std::string modelFile = arg(a, "-load", "");
    if (modelFile.empty())
    {
        std::fprintf(stderr, "[ncc] need -t1 or -load\n");
        printUsage();
        return -1;
    }

    NccMatcher m;
    const int64 tl = cv::getTickCount();
    if (!m.readNccModel(modelFile))
    {
        std::fprintf(stderr, "[ncc] cannot read model: %s\n", modelFile.c_str());
        return -1;
    }
    const double loadMs = (cv::getTickCount() - tl) * 1000.0 / cv::getTickFrequency();

    NccFindParams fp;
    fp.minScore   = argD(a, "-minscore", 0.5);
    fp.maxMatches = (int)argD(a, "-max", 1);
    fp.maxOverlap = argD(a, "-overlap", 0.5);
    fp.subpixel   = (int)argD(a, "-subpix", 1);
    fp.numLevels  = (int)argD(a, "-levels", 0);
    fp.debug      = (int)argD(a, "-debug", 0);

    const std::string one    = arg(a, "-s", "");
    const std::string dir    = arg(a, "-d", "case1");
    const std::string outD   = arg(a, "-o", "case1/out");
    const bool        nodraw = has(a, "-nodraw");

    std::vector<std::string> files;
    if (!one.empty())
        files.push_back(one);
    else
        files = geomatch::listImages(dir, { ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff" });

    if (files.empty())
    {
        std::fprintf(stderr, "[ncc] no image to search\n");
        return -1;
    }
    cv::utils::fs::createDirectory(outD);

    std::printf("[ncc] model    : %s  (%dx%d, %d px, %d levels, %d poses)  load %.1f ms\n",
                modelFile.c_str(), m.modelSize().width, m.modelSize().height,
                m.modelPixelCount(), m.numLevels(), m.angleCount(), loadMs);
    std::printf("[ncc] search   : minScore %.2f  maxMatches %d  maxOverlap %.2f  subpixel %d\n",
                fp.minScore, fp.maxMatches, fp.maxOverlap, fp.subpixel);
    std::printf("%-34s %-6s %10s %10s %10s %10s\n",
                "image", "count", "ncc", "col(x)", "row(y)", "angle_deg");

    double sumMs = 0;
    int    nImg = 0, nHit = 0;
    for (size_t i = 0; i < files.size(); i++)
    {
        cv::Mat img = cv::imread(files[i], cv::IMREAD_GRAYSCALE);
        if (img.empty()) continue;

        const std::vector<NccResult> res = m.findNccModel(img, fp);
        sumMs += m.lastPreprocessTime() + m.lastSearchTime();
        nImg++;
        nHit += (int)res.size();

        std::string base = files[i];
        const size_t sl = base.find_last_of("/\\");
        if (sl != std::string::npos) base = base.substr(sl + 1);

        std::printf("%-34s %-6d", base.c_str(), (int)res.size());
        if (res.empty()) std::printf(" %10s %10s %10s %10s\n", "-", "-", "-", "-");
        for (size_t k = 0; k < res.size(); k++)
        {
            if (k > 0) std::printf("%-34s %-6s", "", "");
            std::printf(" %10.3f %10.2f %10.2f %10.1f\n",
                        res[k].score, res[k].pos.x, res[k].pos.y,
                        res[k].angle * 180.0 / CV_PI);
        }

        if (!nodraw)
        {
            const cv::Mat vis = m.drawResult(img, res);
            if (!vis.empty()) cv::imwrite(outD + "/" + base, vis);
        }
    }

    if (nImg > 0)
        std::printf("----\nimages %d   hits %d   avg %.2f ms/image (preprocess+search)\n",
                    nImg, nHit, sumMs / nImg);
    std::printf("results written to %s\n", outD.c_str());
    return 0;
}


