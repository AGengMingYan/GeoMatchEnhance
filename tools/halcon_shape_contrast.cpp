// halcon_shape_contrast.cpp - does Halcon 17 also report 95 % scores at WRONG
// positions when the contrast threshold is pushed high enough to collapse the
// model point count?
//
// Why this exists:
//   On a textured target, raising 最小对比度 keeps only the strongest edges.
//   Two things then happen at once:
//     * the score is a normalised mean over n points, so a small n makes a
//       coincidental alignment look like 0.95+;
//     * the surviving edges often lie on one line, so the position along that
//       line is unconstrained (aperture problem).
//   The question "is this OUR bug or is it inherent to the algorithm?" can only
//   be answered by running Halcon on the same template, image and contrast.
//   This tool does exactly that: same process, same inputs, same sweep.
//
// Both libraries are x64 here (D:\Develop\Halcon17\lib\x64-win64 + GeoMatch.lib),
// so the two results come from one executable.
//
// Build: tools\build-halcon-shape-contrast.bat
//   -> build_x64\halcon_shape_contrast.exe
//
// usage:
//   halcon_shape_contrast <template> <image> [--gt col,row]
//                         [--levels lo:hi,lo:hi,...] [--min-score F] [-v]
//
//   --gt       true position (column,row) of the template in <image>; matches
//              further than 5 px from it count as FALSE POSITIVES.  Omit it when
//              the template is not in the image at all - then any match is false.
//   --levels   contrast hysteresis pairs in GRAY-VALUE-DIFFERENCE units, i.e.
//              exactly what the GUI's 最小对比度/高阈值 show.  Ours multiplies by
//              kSobelPerGray = 4 internally, so both sides get the same numbers.

#include "HalconCpp.h"
#include "GeoMatch/GeoMatch.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace HalconCpp;

namespace
{
    // GUI's 灰度差 -> internal Sobel scale (see kSobelPerGray in GeoGUIDemo.cpp)
    const double kSobelPerGray = 4.0;

    struct Level { double lo, hi; };

    struct Hit
    {
        double col = 0, row = 0, angleDeg = 0, score = 0;
    };

    HImage toHGray(const std::string& path, cv::Mat& gray8)
    {
        HImage img(path.c_str());
        if (img.CountChannels() > 1)
        {
            HObject g;
            Rgb1ToGray(img, &g);
            img = HImage(g);
        }
        gray8 = cv::imread(path, cv::IMREAD_GRAYSCALE);
        return img;
    }

    //-------------------------------------------------------------------
    //  Halcon side
    //-------------------------------------------------------------------
    bool halconModel(const HImage& tpl, const Level& lv, HTuple& modelID,
                     int& points, std::string& err, const char* opt = "auto")
    {
        try
        {
            HTuple contrast;
            contrast.Append(lv.lo);
            contrast.Append(lv.hi);

            // 360 degrees to match the GUI default; AngleStep 'auto'
            CreateShapeModel(tpl, "auto",
                             -PI, 2.0 * PI, "auto",
                             opt, "use_polarity",
                             contrast, "auto", &modelID);

            // model point count: the level-1 contour is the full-resolution
            // model and its polyline vertices are the sampled edge points
            HObject contours;
            GetShapeModelContours(&contours, modelID, 1);
            HTuple nObj;
            CountObj(contours, &nObj);
            points = 0;
            for (int i = 1; i <= nObj[0].I(); i++)
            {
                HObject one;
                SelectObj(contours, &one, i);
                HTuple r, c;
                GetContourXld(one, &r, &c);
                points += (int)r.Length();
            }
            return true;
        }
        catch (HException& e)
        {
            err = e.ErrorMessage().Text();
            return false;
        }
    }

    bool halconFind(const HImage& img, const HTuple& modelID, double minScore,
                    int numMatches, std::vector<Hit>& out, double& ms,
                    std::string& err)
    {
        out.clear();
        ms = 0;
        try
        {
            const double t0 = (double)cv::getTickCount() / cv::getTickFrequency();
            HTuple row, col, ang, score;
            FindShapeModel(img, modelID, -PI, 2.0 * PI,
                           minScore, numMatches, 0.0,
                           "least_squares", 0, 0.9,
                           &row, &col, &ang, &score);
            ms = ((double)cv::getTickCount() / cv::getTickFrequency() - t0) * 1000.0;

            for (int i = 0; i < (int)score.Length(); i++)
            {
                Hit h;
                h.col = col[i].D();
                h.row = row[i].D();
                h.angleDeg = ang[i].D() * 180.0 / PI;
                h.score = score[i].D();
                out.push_back(h);
            }
            return true;
        }
        catch (HException& e)
        {
            err = e.ErrorMessage().Text();
            return false;
        }
    }

    //-------------------------------------------------------------------
    //  our side (geomatch::Matcher) - parameters mirrored 1:1
    //-------------------------------------------------------------------
    bool oursModel(const cv::Mat& tplGray, const Level& lv, geomatch::Matcher& m,
                   int& points, float& spread, float& normalShare,
                   std::string& err, int minChainLen = 2, int maxPoints = 1200,
                   int scoreMode = 1)
    {
        geomatch::Config c;
        c.angleStart  = -CV_PI;
        c.angleExtent = 2.0 * CV_PI;
        c.minContrast = lv.lo * kSobelPerGray;
        c.contrast    = lv.hi * kSobelPerGray;
        c.maxPoints   = maxPoints;
        c.numLevels   = 0;
        c.metric      = 0;
        c.minChainLen = minChainLen;

        c.minScore    = 0.5;
        c.maxMatches  = 5;
        c.minDistance = 0.0;        // keep duplicates, they ARE the symptom
        c.maxOverlap  = 1.0;        // off
        c.subpixel    = 1;
        c.scoreMode   = scoreMode;
        c.minCoverage = 0.f;

        if (!m.create(tplGray, c)) { err = "create failed"; return false; }
        points      = m.modelPointCount();
        spread      = m.pointSpreadRatio();
        normalShare = m.normalPeakShare();
        return true;
    }

    bool oursFind(const geomatch::Matcher& m, const cv::Mat& imgGray,
                  std::vector<Hit>& out, std::string& err)
    {
        out.clear();
        const std::vector<geomatch::Detection> d = m.detect(imgGray);
        for (size_t i = 0; i < d.size(); i++)
        {
            Hit h;
            h.col = d[i].pos.x;
            h.row = d[i].pos.y;
            h.angleDeg = d[i].angle * 180.0 / CV_PI;
            h.score = d[i].score;
            out.push_back(h);
        }
        (void)err;
        return true;
    }

    const char* truthMark(const Hit& h, bool haveGt, double gtCol, double gtRow,
                          double tol = 5.0)
    {
        if (!haveGt) return "FALSE";       // template is not in the scene at all
        const double d = std::hypot(h.col - gtCol, h.row - gtRow);
        return d <= tol ? "true " : "FALSE";
    }

    int countHigh(const std::vector<Hit>& v, double thr)
    {
        int n = 0;
        for (size_t i = 0; i < v.size(); i++)
            if (v[i].score >= thr) n++;
        return n;
    }

    int countFalse(const std::vector<Hit>& v, bool haveGt, double gtCol,
                   double gtRow, double thr)
    {
        int n = 0;
        for (size_t i = 0; i < v.size(); i++)
            if (v[i].score >= thr &&
                std::string(truthMark(v[i], haveGt, gtCol, gtRow)) == "FALSE")
                n++;
        return n;
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // a crash must still show how far we got

    if (argc < 3)
    {
        std::printf(
            "usage: halcon_shape_contrast <template> <image> [--gt col,row]\n"
            "                             [--levels lo:hi,lo:hi,...] [--min-score F] [-v]\n"
            "  --gt     true position of the template in <image>; omit it when the\n"
            "           template is NOT in the image (then any match is a false positive)\n"
            "  --levels contrast hysteresis pairs in gray-difference units\n");
        return 1;
    }

    const std::string tplPath = argv[1];
    const std::string imgPath = argv[2];
    bool   haveGt  = false;
    double gtCol = 0, gtRow = 0;
    double minScore = 0.5;
    bool   verbose = false;
    int    oursMinChain = 2;
    int    oursMaxPoints = 1200;
    int    oursScoreMode = 1;
    std::string halconOpt = "auto";

    std::vector<Level> levels;
    const double defLo[] = { 3, 8, 13, 20, 25, 30, 40, 60 };
    const double defHi[] = { 5, 10, 17, 25, 30, 36, 50, 72 };
    for (int i = 0; i < 8; i++) levels.push_back({ defLo[i], defHi[i] });

    for (int i = 3; i < argc; i++)
    {
        const std::string a = argv[i];
        if (a == "-v") { verbose = true; continue; }
        if (a == "--gt" && i + 1 < argc)
        {
            double x = 0, y = 0;
            if (std::sscanf(argv[++i], "%lf,%lf", &x, &y) == 2)
            { haveGt = true; gtCol = x; gtRow = y; }
            continue;
        }
        if (a == "--min-score" && i + 1 < argc) { minScore = std::atof(argv[++i]); continue; }
        if (a == "--ours-min-chain" && i + 1 < argc) { oursMinChain = std::atoi(argv[++i]); continue; }
        if (a == "--ours-max-points" && i + 1 < argc) { oursMaxPoints = std::atoi(argv[++i]); continue; }
        if (a == "--ours-score-mode" && i + 1 < argc) { oursScoreMode = std::atoi(argv[++i]); continue; }
        if (a == "--halcon-opt" && i + 1 < argc) { halconOpt = argv[++i]; continue; }
        if (a == "--levels" && i + 1 < argc)
        {
            levels.clear();
            const std::string s = argv[++i];
            size_t p = 0;
            while (p < s.size())
            {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                double lo = 0, hi = 0;
                if (std::sscanf(s.substr(p, q - p).c_str(), "%lf:%lf", &lo, &hi) == 2)
                    levels.push_back({ lo, hi });
                p = q + 1;
            }
        }
    }

    cv::Mat tplGray, imgGray;
    HImage hTpl = toHGray(tplPath, tplGray);
    HImage hImg = toHGray(imgPath, imgGray);
    if (tplGray.empty() || imgGray.empty())
    {
        std::printf("cannot read images\n");
        return 2;
    }

    std::printf("template %s  (%dx%d)\n", tplPath.c_str(), tplGray.cols, tplGray.rows);
    std::printf("image    %s  (%dx%d)\n", imgPath.c_str(), imgGray.cols, imgGray.rows);
    if (haveGt)
        std::printf("truth    true position = col %.1f, row %.1f  (tolerance 5 px)\n",
                    gtCol, gtRow);
    else
        std::printf("truth    template is NOT in this image -> every match is a false positive\n");
    std::printf("minScore %.2f   contrast in gray-difference units (same as the GUI slider)\n\n",
                minScore);

    char line[160];
    for (size_t k = 0; k < levels.size(); k++)
    {
        const Level& lv = levels[k];
        char tag[32];
        std::snprintf(tag, sizeof(tag), "%.0f/%.0f", lv.lo, lv.hi);

        geomatch::Matcher m;
        int ourPts = 0, hPts = 0;
        float spread = 0, nrm = 0;
        std::string err, hErr;
        std::vector<Hit> ourHits, hHits;
        double hMs = 0;

        if (oursModel(tplGray, lv, m, ourPts, spread, nrm, err, oursMinChain,
                      oursMaxPoints, oursScoreMode))
            oursFind(m, imgGray, ourHits, err);

        HTuple modelID;
        if (halconModel(hTpl, lv, modelID, hPts, hErr, halconOpt.c_str()))
        {
            halconFind(hImg, modelID, minScore, 5, hHits, hMs, hErr);
            ClearShapeModel(modelID);
        }

        char o[96] = "-", hh[96] = "-";
        if (!ourHits.empty())
            std::snprintf(o, sizeof(o), "%.3f @(%.1f,%.1f) %s", ourHits[0].score,
                          ourHits[0].col, ourHits[0].row,
                          truthMark(ourHits[0], haveGt, gtCol, gtRow));
        if (!hHits.empty())
            std::snprintf(hh, sizeof(hh), "%.3f @(%.1f,%.1f) %s", hHits[0].score,
                          hHits[0].col, hHits[0].row,
                          truthMark(hHits[0], haveGt, gtCol, gtRow));

        std::snprintf(line, sizeof(line),
                      "%-7s pts %4d spread %5.2f nrm %3.0f%% | ours: %-32s (%d hit) "
                      "| halcon pts %4d: %-32s (%d hit)",
                      tag, ourPts, spread, nrm * 100.0f, o, (int)ourHits.size(),
                      hPts, hh, (int)hHits.size());
        std::printf("%s\n", line);

        if (verbose)
        {
            for (size_t i = 0; i < ourHits.size(); i++)
                std::printf("      ours   %d s=%.3f @(%.1f,%.1f) a=%.2f  %s\n",
                            (int)i, ourHits[i].score, ourHits[i].col, ourHits[i].row,
                            ourHits[i].angleDeg,
                            truthMark(ourHits[i], haveGt, gtCol, gtRow));
            for (size_t i = 0; i < hHits.size(); i++)
                std::printf("      halcon %d s=%.3f @(%.1f,%.1f) a=%.2f  %s\n",
                            (int)i, hHits[i].score, hHits[i].col, hHits[i].row,
                            hHits[i].angleDeg,
                            truthMark(hHits[i], haveGt, gtCol, gtRow));
        }

        if (!hErr.empty()) std::printf("      halcon error: %s\n", hErr.c_str());
        if (!err.empty())  std::printf("      ours error  : %s\n", err.c_str());
    }
    return 0;
}
