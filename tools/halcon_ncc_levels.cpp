//======================================================================
//  halcon_ncc_levels.cpp - print the pyramid level count Halcon ACTUALLY
//  builds for an NCC model (create_ncc_model caps the requested NumLevels).
//
//  Why: a 231x233 template asked for 6 levels still ends up with 5, because
//  Halcon refuses to build a coarsest patch below ~9 px.  Our NccMatch used
//  to allow that 6th level (8x8) and then mis-matched a PCB pad array
//  (0.789, 340 px off) while Halcon returned the true pose at 1.0000 - see
//  README "三个容易踩的坑" and NccMatch.cpp's minSide comment.
//
//  32-bit only (HalconDemo/Halcon17/x86sse2-win32).
//
//  usage:
//    halcon_ncc_levels <template> <requested_levels>
//
//  build:
//    cmd /c tools\build-halcon-ncc-levels.bat
//======================================================================
#include "halconcpp/HalconCpp.h"
#include <cstdio>
#include <cstdlib>

using namespace HalconCpp;

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: halcon_ncc_levels <template> <requested_levels>\n"
                    "   prints the NumLevels Halcon actually used (it caps the request)\n");
        return 1;
    }
    try
    {
        HImage tpl;
        tpl.ReadImage(argv[1]);
        if (tpl.CountChannels().I() != 1) tpl = tpl.Rgb1ToGray();

        const int req = std::atoi(argv[2]);
        HNCCModel m(tpl, req, -M_PI, 2.0 * M_PI, 0.0, "use_polarity");

        double  a0 = 0, aext = 0, astep = 0;
        HString metric;
        const Hlong nl = m.GetNccModelParams(&a0, &aext, &astep, &metric);

        const long w = (long)tpl.Width().I();
        const long h = (long)tpl.Height().I();
        long coarse = (w < h ? w : h);
        for (int i = 1; i < (int)nl; i++) coarse = (coarse + 1) / 2;

        std::printf("tpl=%s  %ldx%ld  requested=%d -> actual NumLevels=%d  "
                    "coarsest~%ldx%ld  angleStep=%.2f deg  extent=%.1f deg\n",
                    argv[1], w, h, req, (int)nl, coarse, coarse,
                    astep * 180.0 / M_PI, aext * 180.0 / M_PI);
    }
    catch (HException& e)
    {
        std::printf("Halcon error: %s\n", e.ErrorMessage().Text());
        return 1;
    }
    return 0;
}
