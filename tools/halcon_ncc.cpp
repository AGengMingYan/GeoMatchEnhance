// halcon_ncc.cpp - Halcon 17 find_ncc_model as ground truth for NccMatch.
//
// Why this exists: cv::matchTemplate cannot rotate, so ncc_verify.exe can only
// prove the score is right at angle 0.  Halcon's find_ncc_model does search
// angles, so it is the only ground truth for the rotated cases.
//
// Both libraries are x64 here, so one process computes both sides on the same
// image, with the same model parameters, and prints them side by side:
//     Halcon  : CreateNccModel / FindNccModel       (D:\Develop\Halcon17)
//     ours    : geomatch::NccMatcher                (NccMatch/)
//
// Build: tools\build-halcon-ncc.bat   (x64, links halconcpp.lib + GeoMatch.lib)
//   -> build_x64\halcon_ncc.exe
//
// usage:
//   halcon_ncc <template> <image> [minScore] [angleExtentDeg]   one image, both impls
//   halcon_ncc rot <template> <theta1> <theta2> ...             synthetic rotated poses
//   halcon_ncc probe <template>                                 which params this build accepts

#include "HalconCpp.h"
#include "NccMatch/NccMatch.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace HalconCpp;

namespace
{
    double nowMs()
    {
        return cv::getTickCount() * 1000.0 / cv::getTickFrequency();
    }

    // Halcon's Row/Column/Angle -> ours.  Halcon reports the reference point,
    // which for create_ncc_model is the centre of the template domain, i.e. the
    // same thing our NccResult.pos holds.
    struct HRes
    {
        double row, col, angleDeg, score, ms;
    };

    bool halconFind(const std::string& tplPath, const std::string& imgPath,
                    double angleStartDeg, double angleExtentDeg, double minScore,
                    int numMatches, HRes& out, std::string& err)
    {
        try
        {
            // NCC is 8-bit single channel; the captures are colour JPGs, so both
            // sides work on Rgb1ToGray == cv::COLOR_BGR2GRAY (BT.601 weights)
            HImage tpl;
            tpl.ReadImage(tplPath.c_str());
            if (tpl.CountChannels() > 1)
            {
                HObject g;
                Rgb1ToGray(tpl, &g);
                tpl = HImage(g);
            }

            HNCCModel model;
            model.CreateNccModel(tpl, Hlong(0),
                                 angleStartDeg * PI / 180.0,
                                 angleExtentDeg * PI / 180.0,
                                 0.0, "use_polarity");

            HImage img;
            img.ReadImage(imgPath.c_str());
            if (img.CountChannels() > 1)
            {
                HObject g;
                Rgb1ToGray(img, &g);
                img = HImage(g);
            }

            // find_ncc_model rejects NumLevels = 0 (error 1307); pass the number
            // of levels the model actually has, which is what 0 means anyway
            double aS = 0, aE = 0, aStep = 0;
            HString metric;
            const Hlong nLev = model.GetNccModelParams(&aS, &aE, &aStep, &metric);

            // the reference point Halcon reports; needed to compare coordinates
            double oRow = 0, oCol = 0;
            model.GetNccModelOrigin(&oRow, &oCol);
            std::printf("halcon model   : %lld levels, angle %.1f..%.1f step %.3f deg, "
                        "metric %s, origin (row %.2f, col %.2f)\n",
                        (long long)nLev, aS * 180.0 / PI, (aS + aE) * 180.0 / PI,
                        aStep * 180.0 / PI, metric.Text(), oRow, oCol);

            HTuple row, col, ang, sco;
            const double t0 = nowMs();
            // NOTE: for NCC, SubPixel is 'true'/'false' - 'interpolation' and
            // 'least_squares' belong to find_shape_model and make this operator
            // fail with #1307 "wrong value of control parameter 7".
            model.FindNccModel(img, angleStartDeg * PI / 180.0,
                               angleExtentDeg * PI / 180.0,
                               minScore, numMatches, 0.5, "true", nLev,
                               &row, &col, &ang, &sco);
            out.ms = nowMs() - t0;

            if (sco.Length() < 1)
            {
                out.score = 0.0;
                out.row = out.col = out.angleDeg = 0.0;
                return false;
            }
            out.row      = row[0].D();
            out.col      = col[0].D();
            out.angleDeg = ang[0].D() * 180.0 / PI;
            out.score    = sco[0].D();
            return true;
        }
        catch (HException& e)
        {
            err = e.ErrorMessage().Text();
            return false;
        }
    }


    struct Ours
    {
        double row, col, angleDeg, score, ms;
        int    hits;
    };

    Ours oursFind(const cv::Mat& tpl, const cv::Mat& img,
                  double angleStartDeg, double angleExtentDeg, double minScore,
                  int numMatches)
    {
        geomatch::NccParams p;
        p.angleStart  = angleStartDeg * CV_PI / 180.0;
        p.angleExtent = angleExtentDeg * CV_PI / 180.0;

        geomatch::NccFindParams fp;
        fp.minScore   = minScore;
        fp.maxMatches = numMatches;
        fp.subpixel   = 1;
        fp.debug      = 0;              // 1 or 2 to print stage timings / score maps

        geomatch::NccMatcher m;
        Ours o;
        o.row = o.col = o.angleDeg = o.score = 0.0;
        o.hits = 0;
        o.ms = 0.0;
        if (!m.createNccModel(tpl, p)) { std::printf("   (createNccModel FAILED)\n"); return o; }

        const double t0 = nowMs();
        const std::vector<geomatch::NccResult> r = m.findNccModel(img, fp);
        o.ms = nowMs() - t0;

        o.hits = (int)r.size();
        if (!r.empty())
        {
            o.col = r[0].pos.x;      // x == column
            o.row = r[0].pos.y;      // y == row
            o.angleDeg = r[0].angle * 180.0 / CV_PI;
            o.score = r[0].score;
        }
        return o;
    }
}

int main(int argc, char** argv)
{
    //---- rot: synthetic ground truth for ROTATED poses -------------------
    // Build a scene with the template pasted in, rotate the WHOLE scene by
    // theta, then ask both matchers for the pose.  A correct rotation search
    // must report score ~1 and angle ~theta at the rotated position.  This is
    // the only way to check the rotated case, because cv::matchTemplate cannot
    // rotate (so ncc_verify.cpp is limited to angle 0).
    if (argc >= 4 && std::string(argv[1]) == "rot")
    {
        const std::string tplPath = argv[2];
        const cv::Mat tpl = cv::imread(tplPath, cv::IMREAD_GRAYSCALE);
        if (tpl.empty()) { std::printf("cannot read template\n"); return 2; }

        // noise background so the pasted template is the only strong feature
        cv::Mat scene(700, 700, CV_8U);
        cv::randu(scene, cv::Scalar(0), cv::Scalar(255));
        const cv::Point at(300, 280);
        tpl.copyTo(scene(cv::Rect(at, tpl.size())));

        const cv::Point2f sc(349.5f, 349.5f);          // scene centre
        const cv::Point2f tc(at.x + (tpl.cols - 1) * 0.5f,
                             at.y + (tpl.rows - 1) * 0.5f);

        std::printf("template %s (%dx%d)  scene 700x700, pasted centre (%.1f,%.1f)\n\n",
                    tplPath.c_str(), tpl.cols, tpl.rows, tc.x, tc.y);
        std::printf("%7s %-8s %8s %9s %9s %9s | %8s %9s %9s %9s\n",
                    "theta", "impl", "score", "col(x)", "row(y)", "angle", "|dscore",
                    "dcol", "drow", "dangle");

        for (int a = 3; a < argc; a++)
        {
            const double theta = atof(argv[a]);

            // rotate the whole scene about its centre
            const cv::Mat M = cv::getRotationMatrix2D(sc, theta, 1.0);
            cv::Mat rot;
            cv::warpAffine(scene, rot, M, scene.size(),
                           cv::INTER_LINEAR, cv::BORDER_REPLICATE);

            // where did the pasted template centre go?
            const double ca = theta * CV_PI / 180.0;
            const double dx = tc.x - sc.x, dy = tc.y - sc.y;
            const double ex = sc.x + dx * std::cos(ca) + dy * std::sin(ca);
            const double ey = sc.y - dx * std::sin(ca) + dy * std::cos(ca);

            const std::string png = "build_x64/_rot_scene.png";
            cv::imwrite(png, rot);

            HRes h;
            std::string err;
            const bool hOk = halconFind(tplPath, png, -180.0, 360.0, 0.3, 1, h, err);
            const Ours o = oursFind(tpl, rot, -180.0, 360.0, 0.3, 1);

            if (hOk)
                std::printf("%7.1f %-8s %8.4f %9.2f %9.2f %9.2f\n",
                            theta, "halcon", h.score, h.col, h.row, h.angleDeg);
            else
                std::printf("%7.1f %-8s   no match %s\n", theta, "halcon", err.c_str());

            if (o.hits > 0)
            {
                double da = 0.0;
                if (hOk)
                {
                    da = o.angleDeg - h.angleDeg;
                    while (da > 180.0)  da -= 360.0;
                    while (da < -180.0) da += 360.0;
                }
                std::printf("%7.1f %-8s %8.4f %9.2f %9.2f %9.2f",
                            theta, "ours", o.score, o.col, o.row, o.angleDeg);
                if (hOk)
                    std::printf(" | %8.4f %9.2f %9.2f %9.2f",
                                o.score - h.score, o.col - h.col, o.row - h.row, da);
                std::printf("   (expect %.2f,%.2f)\n", ex, ey);
            }
            else
                std::printf("%7.1f %-8s   no match\n", theta, "ours");
        }
        return 0;
    }

    if (argc >= 3 && std::string(argv[1]) == "probe")
    {
        // Which (SubPixel, NumLevels) combination does this build accept?
        // find_ncc_model returned #1307 "wrong value of control parameter 7"
        // with every value, including through the typed HNCCModel overload, so
        // try the generic all-HTuple interface where the arguments cannot be
        // mismatched by overload resolution.
        try
        {
            HImage tpl;
            tpl.ReadImage(argv[2]);
            if (tpl.CountChannels() > 1) { HObject g; Rgb1ToGray(tpl, &g); tpl = HImage(g); }

            HTuple modelID;
            CreateNccModel(tpl, HTuple(0), HTuple(-PI), HTuple(2 * PI),
                           HTuple(0), HTuple("use_polarity"), &modelID);
            std::printf("create via generic API OK, modelID = %lld\n",
                        (long long)modelID[0].L());

            const char* sub[] = { "false", "true" };
            for (int s = 0; s < 2; s++)
            {
                for (int nl = 0; nl <= 5; nl++)
                {
                    try
                    {
                        HTuple row, col, ang, sco;
                        FindNccModel(tpl, modelID, HTuple(-PI), HTuple(2 * PI), HTuple(0.5),
                                     HTuple(1), HTuple(0.5), HTuple(sub[s]), HTuple(nl),
                                     &row, &col, &ang, &sco);
                        std::printf("  generic subpixel %-14s numLevels %d -> OK (%lld hits)\n",
                                    sub[s], nl, (long long)sco.Length());
                    }
                    catch (HException& e)
                    {
                        std::printf("  generic subpixel %-14s numLevels %d -> #%ld %s\n",
                                    sub[s], nl, (long)e.ErrorCode(),
                                    e.ErrorMessage().Text());
                    }
                }
            }
        }
        catch (HException& e)
        {
            std::printf("probe failed: #%ld %s\n", (long)e.ErrorCode(),
                        e.ErrorMessage().Text());
        }
        return 0;
    }

    if (argc < 3)
    {
        std::printf("usage: halcon_ncc <template> <image> [minScore] [angleExtentDeg]\n"
                    "   e.g. halcon_ncc case1/temp.jpg case1/tmp_grab_1762994178.jpg 0.5 360\n");
        return 1;
    }
    const std::string tplPath = argv[1];
    const std::string imgPath = argv[2];
    const double minScore    = argc > 3 ? atof(argv[3]) : 0.5;
    const double angleExtent = argc > 4 ? atof(argv[4]) : 360.0;
    const double angleStart  = -angleExtent * 0.5;

    const cv::Mat tpl = cv::imread(tplPath, cv::IMREAD_GRAYSCALE);
    const cv::Mat img = cv::imread(imgPath, cv::IMREAD_GRAYSCALE);
    if (tpl.empty() || img.empty())
    {
        std::printf("cannot read images\n");
        return 2;
    }

    std::printf("template %s  (%dx%d)\n", tplPath.c_str(), tpl.cols, tpl.rows);
    std::printf("image    %s  (%dx%d)\n", imgPath.c_str(), img.cols, img.rows);
    std::printf("params   angle %.1f..%.1f deg   minScore %.2f   matches 1\n\n",
                angleStart, angleStart + angleExtent, minScore);

    HRes h;
    std::string err;
    const bool hOk = halconFind(tplPath, imgPath, angleStart, angleExtent,
                                minScore, 1, h, err);

    const Ours o = oursFind(tpl, img, angleStart, angleExtent, minScore, 1);

    std::printf("%-8s %8s %10s %10s %10s %9s\n",
                "impl", "score", "col(x)", "row(y)", "angle_deg", "ms");
    if (hOk)
        std::printf("%-8s %8.4f %10.2f %10.2f %10.2f %9.1f\n",
                    "halcon", h.score, h.col, h.row, h.angleDeg, h.ms);
    else
        std::printf("%-8s   no match%s%s\n", "halcon",
                    err.empty() ? "" : "   err: ", err.c_str());

    if (o.hits > 0)
        std::printf("%-8s %8.4f %10.2f %10.2f %10.2f %9.1f\n",
                    "ours", o.score, o.col, o.row, o.angleDeg, o.ms);
    else
        std::printf("%-8s   no match\n", "ours");

    if (hOk && o.hits > 0)
    {
        // wrap the angle difference into -180..180 so 179.9 vs -179.9 reads as 0.2
        double da = o.angleDeg - h.angleDeg;
        while (da > 180.0)  da -= 360.0;
        while (da < -180.0) da += 360.0;
        std::printf("\ndiff   score %+.4f   dcol %+.2f   drow %+.2f   dangle %+.2f deg\n",
                    o.score - h.score, o.col - h.col, o.row - h.row, da);
    }
    return 0;
}
