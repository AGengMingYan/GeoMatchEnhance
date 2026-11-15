// HalconCompare.cpp - Halcon 17 shape-based matching benchmark
// Runs Halcon CreateScaledShapeModel / FindScaledShapeModel on the same
// template + search set as the GeoMatch AnalyzeCase tool, so the two can be
// compared side by side (score distribution, pose, timing).
//
// Build (32-bit, matches the HalconDemo 32-bit SDK):
//   call vcvars32.bat
//   cl /nologo /EHsc /MD /O2 HalconCompare.cpp ^
//      /I "HalconDemo\Halcon17\include\halconcpp" ^
//      /I "HalconDemo\Halcon17\include" ^
//      /link /LIBPATH:"HalconDemo\Halcon17\x86sse2-win32" halconcpp.lib
#include "HalconCpp.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>

using namespace HalconCpp;

// NOTE: PI is already #define'd by Halcon's HConst.h, do not redefine it here.

static std::vector<std::string> listImages(const std::string& dir)
{
    std::vector<std::string> out;
    std::string pat = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        size_t dot = lower.find_last_of('.');
        if (dot == std::string::npos) continue;
        std::string ext = lower.substr(dot);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp")
            out.push_back(dir + "\\" + name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

static double nowMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char** argv)
{
    // Unbuffered stdout: Halcon aborts (0xC0000409) instead of throwing when a
    // parameter is rejected, and block-buffered output would be lost on abort.
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 3)
    {
        printf("usage: HalconCompare <template> <search-dir> [minScore] "
               "[angleDeg] [scaleMin] [scaleMax]\n");
        return 1;
    }
    std::string tplPath = argv[1];
    std::string dir     = argv[2];
    double minScore = argc > 3 ? atof(argv[3]) : 0.3;
    double angleDeg = argc > 4 ? atof(argv[4]) : 5.0;
    double scaleMin = argc > 5 ? atof(argv[5]) : 0.95;
    double scaleMax = argc > 6 ? atof(argv[6]) : 1.05;

    // read template
    HObject ho_tpl;
    ReadImage(&ho_tpl, HTuple(tplPath.c_str()));
    HTuple tplW, tplH;
    GetImageSize(ho_tpl, &tplW, &tplH);
    printf("template %s  (%lld x %lld)\n", tplPath.c_str(),
           (long long)tplW[0].L(), (long long)tplH[0].L());

    // create isotropically scaled shape model (mirrors GeoMatch's scale range)
    double a0 = -angleDeg * PI / 180.0;
    double aE = 2.0 * angleDeg * PI / 180.0;
    HTuple modelID;
    double t0 = nowMs();
    CreateScaledShapeModel(ho_tpl, "auto", HTuple(a0), HTuple(aE), "auto",
                           HTuple(scaleMin), HTuple(scaleMax), "auto",
                           "auto", "ignore_color_polarity", "auto", "auto",
                           &modelID);
    double buildMs = nowMs() - t0;
    printf("model created  angle %.1f..%.1f deg  scale %.3f..%.3f  (%.1f ms)\n",
           -angleDeg, angleDeg, scaleMin, scaleMax, buildMs);

    // scan
    std::vector<std::string> files = listImages(dir);
    if (files.empty())
    {
        printf("no images in %s\n", dir.c_str());
        return 2;
    }

    printf("%-30s %9s %10s %10s %9s %8s %8s\n",
           "image", "score", "col(x)", "row(y)", "angle_deg", "scale", "ms");
    int found = 0;
    double totalMs = 0;
    for (size_t i = 0; i < files.size(); i++)
    {
        HObject ho_img;
        ReadImage(&ho_img, HTuple(files[i].c_str()));

        double t1 = nowMs();
        HTuple row, col, angle, scale, score;
        FindScaledShapeModel(ho_img, modelID, HTuple(a0), HTuple(aE),
                             HTuple(scaleMin), HTuple(scaleMax),
                             HTuple(minScore), 1, 0.5,
                             "least_squares", 0, 0.9,
                             &row, &col, &angle, &scale, &score);
        double ms = nowMs() - t1;
        totalMs += ms;

        std::string name = files[i].substr(files[i].find_last_of("\\/") + 1);
        if (score.Length() >= 1 && score[0].D() >= minScore)
        {
            printf("%-30s %9.3f %10.2f %10.2f %9.2f %8.3f %8.1f\n",
                   name.c_str(), score[0].D(), col[0].D(), row[0].D(),
                   angle[0].D() * 180.0 / PI, scale[0].D(), ms);
            found++;
        }
        else
        {
            printf("%-30s %9s %10s %10s %9s %8s %8.1f\n",
                   name.c_str(), "-", "-", "-", "-", "-", ms);
        }
    }
    printf("----\nfound %d/%d  avg %.1f ms/frame\n",
           found, (int)files.size(), totalMs / files.size());
    return 0;
}
