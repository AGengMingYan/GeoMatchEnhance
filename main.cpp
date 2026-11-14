//***********************************************************************
//  Command line front end.
//
//  All of the work is done by the GeoMatch library (GeoMatch/GeoMatch.h);
//  this file only parses the arguments and prints the results.
//
//  the two short helpers (see their comments below):
//    GeoMatchDemo -t1 -t case1/temp.jpg -save case1/model.shm
//                      build a template and write it to a file
//
//    GeoMatchDemo -t2 -load case1/model.shm -s case1/xxx.jpg
//                      load the model, match one image, show the result
//                      (-noshow keeps it from opening a window)
//
//  the full run:
//    GeoMatchDemo -t <template> [-d <dir> | -s <file>] [-o <outdir>] ...
//                 [-save model.shm]            write the model
//                 [-load model.shm]            read a model, skip -t
//                 [-gentest N]                 synthetic angle/scale test
//                 [-selftest 1]                pose round trip check
//                 [-loops N] [-nodraw] [-show] [-debug 1]  (2 = phase timing)
//***********************************************************************
#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#include "GeoMatch/GeoMatch.h"
#include <opencv2/core/utils/filesystem.hpp>

using namespace geomatch;

namespace
{
    std::string gArg(const std::vector<std::string>& a, const std::string& key,
                     const std::string& def)
    {
        for (size_t i = 0; i + 1 < a.size(); i++)
            if (a[i] == key) return a[i + 1];
        return def;
    }

    double gArgD(const std::vector<std::string>& a, const std::string& key, double def)
    {
        std::string v = gArg(a, key, "");
        return v.empty() ? def : atof(v.c_str());
    }

    // a switch that carries no value:  -nodraw   (also accepts -nodraw 0)
    // the next token is only taken as a value when it looks numeric, so
    // that "-t1 -t x.jpg" is not read as "-t1=<garbage>".
    bool gFlag(const std::vector<std::string>& a, const std::string& key, bool def = false)
    {
        for (size_t i = 0; i < a.size(); i++)
        {
            if (a[i] != key) continue;
            if (i + 1 < a.size())
            {
                const std::string& nx = a[i + 1];
                const char c0 = nx.empty() ? '\0' : nx[0];
                const char c1 = nx.size() > 1 ? nx[1] : '\0';
                const bool numeric = std::isdigit((unsigned char)c0) ||
                                     (c0 == '-' && std::isdigit((unsigned char)c1));
                if (numeric) return (int)gArgD(a, key, 1) != 0;
            }
            return true;                 // bare switch, no value follows
        }
        return def;
    }

    // true when the switch is present at all, with or without a value.
    // used to tell "the user asked for this" from "the default was used",
    // which matters for -load: the model carries its own angle/scale range.
    bool gHas(const std::vector<std::string>& a, const std::string& key)
    {
        for (size_t i = 0; i < a.size(); i++)
            if (a[i] == key) return true;
        return false;
    }

    void printStats(const char* name, const Stats& s)
    {
        std::cout << "  " << std::left << std::setw(11) << name
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(7) << s.min  << "  " << std::setw(7) << s.mean
                  << "  " << std::setw(7) << s.max
                  << "  " << std::setw(7) << s.stddev
                  << "  " << std::setw(8) << s.sum << "\n";
    }

    // angle / scale range of the model, shared by t1 and main()
    void applyModelRange(Config& cfg, const std::vector<std::string>& a)
    {
        const double angleExtentDeg = gArgD(a, "-angle", 360.0);
        cfg.angleStart  = -angleExtentDeg * CV_PI / 360.0;
        cfg.angleExtent = angleExtentDeg * CV_PI / 180.0;
        cfg.scaleMin    = gArgD(a, "-smin", 1.0);
        cfg.scaleMax    = gArgD(a, "-smax", 1.0);
    }

    //*******************************************************************
    //  t1 - build a template and write it to a file.
    //
    //      GeoMatchDemo -t1 -t case1/temp.jpg -save case1/model.shm
    //
    //  this is the "offline" step, it is run once for a part.
    //*******************************************************************
    int t1(const std::vector<std::string>& a)
    {
        const std::string templ = gArg(a, "-t", "case1/temp.jpg");
        const std::string model = gArg(a, "-save", "case1/model.shm");

        Config cfg;
        cfg.minContrast = gArgD(a, "-contrast", 12.0);
        cfg.contrast    = gArgD(a, "-hi", 30.0);
        cfg.maxPoints   = (int)gArgD(a, "-points", 1200.0);
        cfg.minChainLen = (int)gArgD(a, "-chain", 2.0);
        if (gHas(a, "-levels")) cfg.numLevels = (int)gArgD(a, "-levels", 0);
        applyModelRange(cfg, a);

        const std::string maskFile = gArg(a, "-mask", "");
        if (!maskFile.empty())
        {
            cfg.mask = cv::imread(maskFile, cv::IMREAD_GRAYSCALE);
            if (cfg.mask.empty())
            {
                std::cerr << "[t1] cannot open mask: " << maskFile << "\n";
                return -1;
            }
        }

        cv::Mat tpl = cv::imread(templ, cv::IMREAD_GRAYSCALE);
        if (tpl.empty())
        {
            std::cerr << "[t1] cannot open template: " << templ << "\n";
            return -1;
        }

        Matcher m;
        const int64 t0 = cv::getTickCount();
        if (!m.create(tpl, cfg))
        {
            std::cerr << "[t1] failed to create the shape model\n";
            return -1;
        }
        const double buildMs =
            (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();

        if (!m.save(model))
        {
            std::cerr << "[t1] failed to save model to " << model << "\n";
            return -1;
        }

        std::cout << "[t1] template : " << templ
                  << "  (" << tpl.cols << "x" << tpl.rows << ")\n"
                  << "[t1] model    : " << model << "\n"
                  << "[t1] points   : " << m.modelPointCount() << "\n"
                  << "[t1] levels   : " << m.numLevels() << "\n"
                  << "[t1] angle    : " << (int)(cfg.angleStart * 180 / CV_PI) << " .. "
                  << (int)((cfg.angleStart + cfg.angleExtent) * 180 / CV_PI) << " deg\n"
                  << "[t1] scale    : " << cfg.scaleMin << " .. " << cfg.scaleMax << "\n"
                  << "[t1] build    : " << std::fixed << std::setprecision(1)
                  << buildMs << " ms\n"
                  << "[t1] done\n";
        return 0;
    }

    //*******************************************************************
    //  t2 - load a model, match one image and show the result.
    //
    //      GeoMatchDemo -t2 -load case1/model.shm -s case1/xxx.jpg
    //
    //  this is the "online" step, it runs per image.  the annotated
    //  image is shown in a window (press any key to close) and also
    //  written to <outDir>/t2_result.png.
    //*******************************************************************
    int t2(const std::vector<std::string>& a)
    {
        const std::string model  = gArg(a, "-load", "case1/model.shm");
        const std::string outDir = gArg(a, "-o", "case1/out");
        std::string image = gArg(a, "-s", "");

        //---------------- load the model --------------------------------
        Matcher m;
        const int64 t0 = cv::getTickCount();
        if (!m.load(model))
        {
            std::cerr << "[t2] cannot load model: " << model << "\n";
            return -1;
        }
        const double loadMs =
            (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();

        //---------------- read the image --------------------------------
        if (image.empty())
        {
            std::vector<std::string> fs = listImages(gArg(a, "-d", "case1"));
            fs = excludeFile(fs, gArg(a, "-t", "case1/temp.jpg"));
            if (fs.empty())
            {
                std::cerr << "[t2] no image to search (use -s or -d)\n";
                return -1;
            }
            image = fs[0];
        }

        const int64 t1tick = cv::getTickCount();
        cv::Mat img = cv::imread(image, cv::IMREAD_GRAYSCALE);
        const double readMs =
            (cv::getTickCount() - t1tick) * 1000.0 / cv::getTickFrequency();
        if (img.empty())
        {
            std::cerr << "[t2] cannot open image: " << image << "\n";
            return -1;
        }

        //---------------- match -----------------------------------------
        Config cfg = m.config();          // keeps the model's own ranges
        cfg.minScore   = gArgD(a, "-minscore", 0.7);
        cfg.maxMatches = (int)gArgD(a, "-max", 12);
        cfg.useOpenCL  = (int)gArgD(a, "-ocl", 1) != 0;
        cfg.debug      = (int)gArgD(a, "-debug", 0);
        cfg.scoreMode  = (int)gArgD(a, "-score-mode", 0);
        cfg.minCoverage = (float)gArgD(a, "-min-cover", 0.0);
        // -skip-w 0 关闭"跳过弱梯度点"；默认 0.9（对齐 Halcon，见 FindParams::skipWeight）
        cfg.skipWeight = (float)gArgD(a, "-skip-w", 0.9);
        // -cell-miss 0 关闭"模板局部缺失"拒绝；默认 0.35（见 FindParams::cellMissMax）
        cfg.cellMissMax = (float)gArgD(a, "-cell-miss", 0.35);
        // minDistance only when asked for, so the value stored in the model wins
        if (gHas(a, "-min-dist")) cfg.minDistance = gArgD(a, "-min-dist", cfg.minDistance);
        cfg.metric     = (int)gArgD(a, "-metric", 0);
        cfg.maxOverlap = gArgD(a, "-max-overlap", 1.0);
        cfg.subpixel   = (int)gArgD(a, "-subpix", 1);
        cfg.boxMode    = (int)gArgD(a, "-box", 0);
        cfg.showContour   = !gFlag(a, "-no-contour");
        cfg.showPoints    = gFlag(a, "-show-points");
        cfg.showGradients = gFlag(a, "-show-grads");
        m.setConfig(cfg);

        Timing tm;
        std::vector<Detection> dets = m.detect(img, &tm);

        std::cout << "[t2] model    : " << model
                  << "  (" << m.modelGray().cols << "x" << m.modelGray().rows
                  << ", " << m.modelPointCount() << " points)\n"
                  << "[t2] image    : " << image
                  << "  (" << img.cols << "x" << img.rows << ")\n"
                  << "[t2] load     : " << std::fixed << std::setprecision(1)
                  << loadMs << " ms   imread " << readMs << " ms\n"
                  << "[t2] match    : preprocess " << tm.preprocessMs
                  << " ms   search " << tm.searchMs
                  << " ms   total " << tm.recognitionMs() << " ms\n"
                  << "[t2] matches  : " << dets.size() << "\n";
        for (size_t k = 0; k < dets.size(); k++)
        {
            const Detection& d = dets[k];
            std::cout << "        #" << k
                      << "  score=" << std::setprecision(3) << d.score;
            // pos is sub-pixel (the sub-pixel refinement returns fractions), so it
            // must not be truncated to int here - that hid the whole point of
            // subpixel=2.
            std::cout << "  pos=(" << std::fixed << std::setprecision(2)
                      << d.pos.x << ";" << d.pos.y << ")"
                      // angle/scale 必须用定点小数：默认浮点格式下
                      // setprecision(1) 只有 1 位有效数字，14.77 会被打成
                      // "1e+01"、45 会被打成 "5e+01"，看起来像几度的误差。
                      << "  angle=" << std::setprecision(2) << (d.angle * 180.0 / CV_PI)
                      << "deg  scale=" << std::setprecision(3) << d.scale;
            // cover 放在**行尾**：中间插入会打断所有既有脚本的正则
            // （`#0 score=... pos=(...;...) angle=...deg`）。
            if (d.coverage > 0.f)
                std::cout << "  cover=" << std::fixed << std::setprecision(2) << d.coverage;
            std::cout << "\n";
        }
        if (dets.empty()) std::cout << "        (nothing found)\n";

        //---------------- show the result -------------------------------
        cv::Mat vis = m.drawResult(img, dets);

        cv::utils::fs::createDirectory(outDir);
        const std::string png = outDir + "/t2_result.png";
        cv::imwrite(png, vis);
        std::cout << "[t2] written  : " << png << "\n";

        if (!gFlag(a, "-noshow"))
        {
            // a headless build has no window support and throws here
            try
            {
                cv::imshow("t2 - match result", vis);
                std::cout << "[t2] press any key in the window to close...\n";
                cv::waitKey(0);
                cv::destroyAllWindows();
            }
            catch (const cv::Exception& e)
            {
                std::cout << "[t2] no window available (" << e.what()
                          << ")\n[t2] result is in " << png << "\n";
            }
        }
        return 0;
    }
}

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);

    // unbuffered: a crash would otherwise swallow everything printed so far
    setvbuf(stdout, nullptr, _IONBF, 0);

    //---------------- configuration -------------------------------------
    Config cfg;
    const std::string templ     = gArg(args, "-t", "case1/temp.jpg");
    const std::string searchDir = gArg(args, "-d", "case1");
    const std::string searchFile = gArg(args, "-s", "");
    const std::string outDir    = gArg(args, "-o", "case1/out");
    const std::string saveModel = gArg(args, "-save", "");
    const std::string loadModel = gArg(args, "-load", "");
    const std::string maskFile  = gArg(args, "-mask", "");

    cfg.minScore    = gArgD(args, "-minscore", 0.7);
    cfg.maxMatches  = (int)gArgD(args, "-max", 12);
    cfg.minContrast = gArgD(args, "-contrast", 12.0);
    cfg.contrast    = gArgD(args, "-hi", 30.0);
    cfg.useOpenCL   = (int)gArgD(args, "-ocl", 1) != 0;
    cfg.debug       = (int)gArgD(args, "-debug", 0);
    cfg.scoreMode   = (int)gArgD(args, "-score-mode", 0);
    cfg.minCoverage = (float)gArgD(args, "-min-cover", 0.0);
    // -skip-w / -cell-miss 原来只在 t2 路径解析，默认路径被静默忽略 ——
    // 于是 `-skip-w 0` 的 A/B 会得到"分数完全不变"的假结论（2026-11-15 排查
    // 多结果高分时踩到）。这里补上，且只在显式给出时覆盖，未指定就沿用
    // Config 的默认值（默认值只应有一处定义，所以回落值直接取 cfg 当前值）。
    if (gHas(args, "-skip-w"))    cfg.skipWeight  = (float)gArgD(args, "-skip-w", cfg.skipWeight);
    if (gHas(args, "-cell-miss")) cfg.cellMissMax = (float)gArgD(args, "-cell-miss", cfg.cellMissMax);
    if (gHas(args, "-min-dist")) cfg.minDistance = gArgD(args, "-min-dist", cfg.minDistance);
    cfg.metric      = (int)gArgD(args, "-metric", 0);
    cfg.maxOverlap  = gArgD(args, "-max-overlap", 1.0);
    cfg.subpixel    = (int)gArgD(args, "-subpix", 1);
    cfg.boxMode     = (int)gArgD(args, "-box", 0);
    cfg.showContour   = !gFlag(args, "-no-contour");
    cfg.showPoints    = gFlag(args, "-show-points");
    cfg.showGradients = gFlag(args, "-show-grads");

    const double angleExtentDeg = gArgD(args, "-angle", 360.0);
    applyModelRange(cfg, args);

    if (!maskFile.empty())
    {
        cfg.mask = cv::imread(maskFile, cv::IMREAD_GRAYSCALE);
        if (cfg.mask.empty())
        {
            std::cerr << "cannot open mask: " << maskFile << "\n";
            return -1;
        }
    }

    const int   loops   = std::max(1, (int)gArgD(args, "-loops", 1));
    const int   gentest = std::max(0, (int)gArgD(args, "-gentest", 0));
    const int   selftest = (int)gArgD(args, "-selftest", 0);
    const bool  show    = gFlag(args, "-show");
    const bool  nodraw  = gFlag(args, "-nodraw");

    //---------------- thread pool ----------------------------------------
    // -threads N   pin the OpenCV thread pool, to see how it scales.
    // applies to every mode, including -t1 / -t2 / -bench
    const int threads = (int)gArgD(args, "-threads", 0);
    if (threads > 0) cv::setNumThreads(threads);

    //---------------- the two short helpers ------------------------------
    // -t1  build + save a model      -t2  load + match one image + show
    if (gFlag(args, "-t1")) return t1(args);
    if (gFlag(args, "-t2")) return t2(args);

    //---------------- machine comparison ---------------------------------
    // -bench   micro benchmark: cores, bandwidth, Sobel, half unpack
    if (gFlag(args, "-bench"))
    {
        std::cout << "threads  : " << cv::getNumThreads() << "\n";
        std::cout << benchmarkReport();
        return 0;
    }

    //---------------- environment ---------------------------------------
    if (!enableOpenCL(cfg.useOpenCL)) cfg.useOpenCL = false;
    std::cout << environmentReport();

    //---------------- model ---------------------------------------------
    Matcher matcher;
    bool modelFromFile = !loadModel.empty();

    if (modelFromFile)
    {
        int64 t0 = cv::getTickCount();
        if (!matcher.load(loadModel))
        {
            std::cerr << "failed to read shape model: " << loadModel << "\n";
            return -1;
        }
        const cv::Mat mg = matcher.modelGray();
        // remember what the model itself searches, before the CLI overrides are
        // merged in; the warning below must report the model's own range
        const Config modelCfg = matcher.config();
        std::cout << "model loaded from " << loadModel
                  << "  (" << mg.cols << "x" << mg.rows
                  << ")  points = " << matcher.modelPointCount()
                  << "  levels = " << matcher.numLevels()
                  << "  time = " << std::fixed << std::setprecision(1)
                  << (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency() << " ms\n";

        // The model carries its own angle/scale range, but every SEARCH setting
        // has to come from the command line.  Without this the -minscore /
        // -score-mode / -min-cover / -max values were parsed and then thrown
        // away, because Matcher::load() resets the config to its defaults.
        Config merged = matcher.config();
        merged.minScore    = cfg.minScore;
        merged.maxMatches  = cfg.maxMatches;
        merged.minContrast = cfg.minContrast;
        merged.contrast    = cfg.contrast;
        merged.useOpenCL   = cfg.useOpenCL;
        merged.debug       = cfg.debug;
        merged.scoreMode   = cfg.scoreMode;
        merged.minCoverage = cfg.minCoverage;
        merged.minDistance = cfg.minDistance;
        merged.metric      = cfg.metric;
        merged.maxOverlap  = cfg.maxOverlap;
        merged.subpixel    = cfg.subpixel;
        merged.boxMode     = cfg.boxMode;
        merged.showContour   = cfg.showContour;
        merged.showPoints    = cfg.showPoints;
        merged.showGradients = cfg.showGradients;
        // the search range is only overridden when it was asked for explicitly
        if (gHas(args, "-angle"))
        {
            merged.angleStart  = cfg.angleStart;
            merged.angleExtent = cfg.angleExtent;
        }
        if (gHas(args, "-smin")) merged.scaleMin = cfg.scaleMin;
        if (gHas(args, "-smax")) merged.scaleMax = cfg.scaleMax;
        matcher.setConfig(merged);

        // The angle/scale grids are fixed when the model is created, so asking
        // for a different range after -load cannot take effect.  Say so instead
        // of silently searching the model's own range.
        if (gHas(args, "-angle") || gHas(args, "-smin") || gHas(args, "-smax"))
        {
            std::cerr << "[warn] -angle / -smin / -smax do not apply to a loaded model "
                         "(the angle and scale grids are built when the model is created). "
                         "Rebuild it with -t1 and the range you need.\n"
                      << "       model currently searches angle "
                      << (int)(modelCfg.angleStart * 180 / CV_PI) << ".."
                      << (int)((modelCfg.angleStart + modelCfg.angleExtent) * 180 / CV_PI)
                      << " deg, scale " << modelCfg.scaleMin << ".." << modelCfg.scaleMax << "\n";
        }
    }
    else
    {
        cv::Mat tpl = cv::imread(templ, cv::IMREAD_GRAYSCALE);
        if (tpl.empty())
        {
            std::cerr << "cannot open template: " << templ << "\n";
            return -1;
        }
        int64 t0 = cv::getTickCount();
        if (!matcher.create(tpl, cfg))
        {
            std::cerr << "failed to create the shape model\n";
            return -1;
        }
        std::cout << "model created from " << templ
                  << "  (" << tpl.cols << "x" << tpl.rows
                  << ")  points = " << matcher.modelPointCount()
                  << "  levels = " << matcher.numLevels()
                  << "  time = " << std::fixed << std::setprecision(1)
                  << (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency() << " ms\n";
    }

    if (!saveModel.empty())
    {
        if (matcher.save(saveModel)) std::cout << "model saved to " << saveModel << "\n";
        else                          std::cerr << "failed to save model to " << saveModel << "\n";
    }

    //---------------- self test -----------------------------------------
    if (selftest == 1)
    {
        std::vector<std::string> files =
            searchFile.empty() ? listImages(searchDir) : std::vector<std::string>{ searchFile };
        if (files.empty()) { std::cerr << "self test: no image\n"; return -1; }
        cv::Mat img = cv::imread(files[0], cv::IMREAD_GRAYSCALE);
        if (img.empty()) { std::cerr << "self test: no image\n"; return -1; }

        for (const Matcher::SelfTestResult& r : matcher.runSelfTest(img, cfg))
        {
            std::cout << "[selftest " << r.variant << "] truth=("
                      << (int)r.truthPos.x << "," << (int)r.truthPos.y
                      << ") a=" << r.truthAngleDeg << " s=" << r.truthScale;
            if (r.found)
                std::cout << " -> found=(" << (int)r.foundPos.x << "," << (int)r.foundPos.y
                          << ") a=" << r.foundAngleDeg << " s=" << r.foundScale
                          << " score=" << r.score << "  err=" << r.errPx;
            else
                std::cout << " -> NOT FOUND";
            std::cout << "\n           probe@truth = " << r.probeAtTruth << "\n";
        }
        return 0;
    }

    //---------------- write the model visualisation ---------------------
    if (!nodraw)
    {
        cv::Mat info = matcher.drawModelInfo(modelFromFile ? loadModel : templ);
        if (!info.empty()) cv::imwrite(outDir + "/model_info.png", info);
    }

    //---------------- images to search ----------------------------------
    std::vector<std::string> images =
        searchFile.empty() ? listImages(searchDir) : std::vector<std::string>{ searchFile };
    images = excludeFile(images, templ);

    //---------------- synthetic angle / scale test ----------------------
    if (gentest > 0)
    {
        GenTestReport rep = matcher.runGenTest(outDir, gentest, cfg);

        std::cout << "\n[gentest] " << rep.total << " synthetic images, "
                  << "model angle extent = " << angleExtentDeg
                  << " deg, scale = " << cfg.scaleMin << ".." << cfg.scaleMax
                  << "\n-----------------------------------------------------------"
                  << "----------------------------------------------------\n"
                  << "  truth              found              score   err-a    "
                  << "err-s   err-px   t(ms)\n"
                  << "-----------------------------------------------------------"
                  << "----------------------------------------------------\n";
        for (const GenTestReport::Case& c : rep.cases)
        {
            std::cout << "  a=" << std::setw(4) << (int)c.truthAngleDeg
                      << " s=" << std::setw(3) << (int)std::round(c.truthScale * 100);
            if (!c.found) { std::cout << "        --- NOT FOUND ---\n"; continue; }
            std::cout << "         a=" << std::fixed << std::setprecision(1)
                      << std::setw(6) << c.angleDeg
                      << " s=" << std::setprecision(3) << std::setw(5) << c.scale
                      << "    " << std::setprecision(3) << c.score
                      << "  " << std::setw(6) << std::setprecision(2)
                      << (c.angleDeg - c.truthAngleDeg) << "deg"
                      << "  " << std::setw(6) << std::setprecision(3) << c.errScale
                      << "  " << std::setw(6) << std::setprecision(2) << c.errPx
                      << "  " << std::setw(6) << std::setprecision(2) << c.ms << "\n";
        }
        std::cout << "-----------------------------------------------------------"
                  << "----------------------------------------------------\n"
                  << "  errors  | mean angle = " << std::fixed << std::setprecision(3)
                  << rep.meanErrAngleDeg << " deg,  max = " << rep.maxErrAngleDeg << " deg\n"
                  << "           | mean scale = " << rep.meanErrScale
                  << ",         max = " << rep.maxErrScale << "\n"
                  << "           | mean pos   = " << rep.meanErrPx
                  << " px,        max = " << rep.maxErrPx << " px\n"
                  << "  scores  | mean = " << rep.meanScore
                  << "        min = " << rep.minScore << "\n";
        if (rep.timing.n)
            std::cout << "  gentest timing [ms] | n=" << rep.timing.n
                      << "  min=" << rep.timing.min << "  mean=" << rep.timing.mean
                      << "  max=" << rep.timing.max << "  stddev=" << rep.timing.stddev
                      << "  total=" << rep.timing.sum << "\n";
        return 0;
    }

    //---------------- batch matching ------------------------------------
    BatchReport rep = matcher.runBatch(
        images, outDir, !nodraw, loops,
        [](const BatchProgress& p)
        {
            std::cout << "--------------------------------------------------------\n"
                      << p.file << "\n"
                      << "  matches = " << p.matches
                      << "   preprocess = " << std::fixed << std::setprecision(1)
                      << p.preprocessMs << " ms"
                      << "   search = " << p.searchMs << " ms"
                      << "   total = " << p.preprocessMs + p.searchMs << " ms\n";
            if (p.detections)
                for (size_t k = 0; k < p.detections->size(); k++)
                {
                    const Detection& d = (*p.detections)[k];
                    printf("    #%zu  s=%.3f", k, d.score);
                    if (d.coverage > 0.f) printf("  c=%.2f", d.coverage);
                    printf("  a=%.1fdeg  k=%.3f  pos=(%.2f;%.2f)\n",
                           d.angle * 180.0 / CV_PI, d.scale,
                           d.pos.x, d.pos.y);
                }
        });

    if (rep.images > 0)
    {
        std::cout << "========================================================\n"
                  << "timing over " << rep.images << " image(s) [ms]\n"
                  << std::fixed << std::setprecision(2)
                  << "              min      mean     max     stddev      sum\n";
        printStats("preprocess",  rep.preprocess);
        printStats("search",      rep.search);
        printStats("total",       rep.total);
        std::cout << "  (recognition time = preprocess + search, per image)\n";
        if (rep.read.n)
        {
            std::cout << "  -----------------------------------------------\n"
                      << "  not included above:\n";
            printStats("imread",     rep.read);
            printStats("draw+save",  rep.draw);
        }
    }

    if (show && !rep.detections.empty())
    {
        cv::Mat img = cv::imread(rep.files[0], cv::IMREAD_GRAYSCALE);
        if (!img.empty())
        {
            cv::imshow("result", matcher.drawResult(img, rep.detections[0]));
            cv::waitKey(0);
        }
    }

    std::cout << "results written to " << outDir << "\n";
    return 0;
}