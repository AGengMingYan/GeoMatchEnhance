//***********************************************************************
//  NccMatch - implementation of the NCC facade (Halcon create_ncc_model /
//  find_ncc_model semantics).
//
//  ---------------------------------------------------------------------
//  THE SCORE
//  ---------------------------------------------------------------------
//  For a pose (x, y, angle) the classic normalised cross correlation is
//      NCC = [ n*SIT - SI*ST ] / ( sqrt(n*SI2 - SI*SI) * sqrt(n*ST2 - ST*ST) )
//  with, over the n sampled pixels of the model region,
//      SI = sum I,  SI2 = sum I^2,  SIT = sum I*T,
//      ST = sum T,  ST2 = sum T^2.
//
//  The template is kept ZERO MEAN (Tm = T - mean(T)), so ST == 0 exactly and
//  the formula collapses to
//      NCC = n * SIT / ( sqrt(n*SI2 - SI*SI) * sqrt(n*ST2) )
//  which means the only per-angle work is one filter2D for SIT: SI and SI2 do
//  not depend on the template at all and are precomputed once per level from
//  the integral images.  That is what makes an angle sweep affordable.
//
//  ---------------------------------------------------------------------
//  WHAT "ROTATION" MEANS HERE
//  ---------------------------------------------------------------------
//  The template is rotated into a CANVAS that is large enough for every angle
//  of the requested range (its size is therefore constant across angles, which
//  is what lets the image-side sums be precomputed once per level).  The sums
//  themselves only run over the ROTATED TEMPLATE (the canvas padding is masked
//  out), so the score is NCC over exactly the rotated template area - what you
//  get by physically rotating a photo and correlating it - and it is 1.000 when
//  the template is pasted into the image at the right pose.
//
//  Two details make that exact rather than approximate, both verified with
//  _ai_tmp/ncc_selfcheck.cpp against cv::matchTemplate(TM_CCOEFF_NORMED):
//    * the canvas keeps the PARITY of the template, so an identity rotation is
//      an integer translation and warpAffine does not resample the template
//    * the valid region is warped from the template (or from the caller's mask)
//      instead of being the whole canvas
//***********************************************************************
#include "NccMatch/NccMatch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace geomatch
{

namespace
{
    const double kPi = 3.14159265358979323846;

    inline double clampD(double v, double lo, double hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    inline double wrapAngle(double a)
    {
        while (a > kPi)  a -= 2 * kPi;
        while (a < -kPi) a += 2 * kPi;
        return a;
    }

    inline double angleDiff(double a, double b)
    {
        return std::fabs(wrapAngle(a - b));
    }
}

// The model state and the per-level record must be at namespace scope: Impl is
// declared as a NESTED type of NccMatcher in the header (pimpl), so defining it
// inside an anonymous namespace would create an unrelated type.

//-------------------------------------------------------------------------
//  one pyramid level of the model
//-------------------------------------------------------------------------
struct LevelData
{
    cv::Mat  gray;                  // CV_8U template at this level
    cv::Mat  mask;                  // CV_8U or empty
    cv::Size canvas;                // rotated bounding box, constant over angles
    cv::Point2f srcCentre;          // template centre in template coordinates
    cv::Point2f centre;             // canvas centre (where srcCentre lands)
    int      n = 0;                 // canvas pixels
    double   angleStep = 0;         // radians, already multiplied by 2^level
    std::vector<double> angles;     // the grid actually scanned at this level
    int      usedPixels = 0;        // template pixels that pass the mask

    // per search: size of the score map (cols/rows of positions)
    int      mapW = 0, mapH = 0;
};

// A template rotated into canvas space.  Built once per (level, angle index)
// and, because rotation depends only on the MODEL (not the search image),
// cached across findNccModel() calls inside Impl::rotCache.  On a video stream
// whose angle barely moves, the second frame onward reuses every entry and the
// warpAffine work (the dominant refine cost) drops to ~zero.
struct RotTpl
{
    cv::Mat tm, mk;
    double  st2 = 0;
    int     nValid = 0;
    bool    ready = false;
    // per-row valid column interval [c0, c1] of the rotated mask; only used
    // when spanOk (a user-painted mask may have holes)
    std::vector<int> c0, c1;
    bool    spanOk = false;
};

struct NccMatcher::Impl
{
    std::vector<LevelData> levels;
    cv::Mat    gray0;
    double     angleStep0 = 0;
    mutable double lastPre = 0, lastSearch = 0;

    // Cross-call LRU of rotated templates.  Key packs (level, angle index).
    // findNccModel() is const and promises concurrent reads, so the cache is
    // only ever MUTATED from the serial pre-build section of refine (never
    // from a parallel_for_ worker); the parallel scoring below only reads the
    // pointers handed out here.  When it fills up it is cleared wholesale,
    // the same no-lock policy the shape matcher uses (an eviction mid-search
    // could invalidate a reference a worker is about to read).
    mutable std::unordered_map<uint64_t, RotTpl> rotCache;
    size_t   rotCacheLimit = 256;
    mutable int rotCacheHits = 0, rotCacheMiss = 0;
};

namespace
{

    //-------------------------------------------------------------------------
    //  Canvas large enough for every angle of the requested range.  Its size is
    //  constant over the sweep, so the score map of every angle has the same
    //  size and the image-side prepare step runs once per level, not per angle.
    //-------------------------------------------------------------------------
    cv::Size canvasForAngleRange(int w, int h, double start, double extent)
    {
        double mw, mh;
        if (extent >= 2 * kPi - 1e-6)
        {
            const double diag = std::sqrt((double)w * w + (double)h * h);
            mw = mh = diag;
        }
        else
        {
            mw = mh = 0;
            const int N = 64;
            for (int i = 0; i <= N; i++)
            {
                const double a = start + extent * i / (double)N;
                const double c = std::fabs(std::cos(a)), s = std::fabs(std::sin(a));
                mw = std::max(mw, w * c + h * s);
                mh = std::max(mh, w * s + h * c);
            }
        }
        // The canvas keeps the PARITY of the template.  Then
        //     (canvas-1)/2 - (template-1)/2  ==  (canvas - template)/2
        // is an integer, so an identity rotation is a pure integer translation
        // and warpAffine COPIES the template instead of resampling it.
        //
        // Forcing an odd canvas instead (`ceil(...) | 1`) put an even sized
        // template half a pixel off centre; bilinear interpolation then smeared
        // it, which alone dropped a pasted-in template from 1.000 to 0.475
        // (40x30), 0.702 (40x31) and 0.652 (41x30) - measured with
        // _ai_tmp/ncc_selfcheck.cpp.
        int cw = (int)std::ceil(mw);
        int ch = (int)std::ceil(mh);
        cw = std::max(cw, w);  if (((cw ^ w) & 1) != 0) cw++;
        ch = std::max(ch, h);  if (((ch ^ h) & 1) != 0) ch++;
        return cv::Size(std::max(cw, 3), std::max(ch, 3));
    }

    //-------------------------------------------------------------------------
    //  Rotate the template into the (constant size) canvas and return
    //    tm     zero-mean template, zero outside the valid region
    //    mk     1.0 inside the valid region, 0 outside (CV_32F)
    //    st2    sum of tm^2 over the valid region
    //    nValid number of valid pixels
    //
    //  The score must only use the VALID pixels: the canvas is expanded so that
    //  large angles do not clip the template, and counting the empty corners
    //  would dilute the correlation (measured 0.99 -> 0.70 at 45 degrees).
    //
    //  getRotationMatrix2D keeps `centre` fixed and that centre is in SOURCE
    //  coordinates, so we rotate about the template centre and then translate it
    //  onto the canvas centre.  Because the canvas has the template's parity the
    //  translation is always an integer number of pixels, which keeps angle 0
    //  resampling-free (and exact, not just approximately right).
    //-------------------------------------------------------------------------
    void buildRotTemplate(const LevelData& ld, double angle,
                          cv::Mat& tm, cv::Mat& mk, double& st2, int& nValid,
                          std::vector<int>* span0 = nullptr,
                          std::vector<int>* span1 = nullptr,
                          bool* spanOk = nullptr)
    {
        cv::Mat M = cv::getRotationMatrix2D(ld.srcCentre, angle * 180.0 / kPi, 1.0);
        M.at<double>(0, 2) += (double)(ld.centre.x - ld.srcCentre.x);
        M.at<double>(1, 2) += (double)(ld.centre.y - ld.srcCentre.y);

        cv::Mat warped;
        cv::warpAffine(ld.gray, warped, M, ld.canvas,
                       cv::INTER_LINEAR, cv::BORDER_REPLICATE);

        // The valid region is always the ROTATED TEMPLATE, never the whole
        // canvas.  For a wide angle range the canvas is padded out to the
        // diagonal, and letting that padding into the sums drags every score
        // down (measured: a template pasted into the image scored 0.206 instead
        // of 1.000 with a 360 degree model).  When the caller supplied no mask
        // the region is built by warping an all-255 image of the template, which
        // costs the same as the template warp above.
        cv::Mat wm;
        if (!ld.mask.empty())
        {
            cv::warpAffine(ld.mask, wm, M, ld.canvas,
                           cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
        }
        else
        {
            const cv::Mat ones(ld.gray.size(), CV_8U, cv::Scalar(255));
            cv::warpAffine(ones, wm, M, ld.canvas,
                           cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
        }

        //-----------------------------------------------------------------
        //  Fused build.  This used to be
        //      countNonZero / convertTo / convertTo / mean(masked) /
        //      subtract / setTo(compare temp) / mul + sum
        //  i.e. eight full-canvas passes with two big temporaries, per angle.
        //  Measured on a 214x214 canvas that alone was 6.6 ms at level 0 -
        //  four times the cost of scoring every candidate!  Two plain passes
        //  (mean, then fill tm/mk/st2/row-span together) do the same arithmetic
        //  with no temporaries.
        //-----------------------------------------------------------------
        const int rows = ld.canvas.height, cols = ld.canvas.width;

        double sum = 0;
        int    n   = 0;
        for (int r = 0; r < rows; r++)
        {
            const uchar* pw = wm.ptr<uchar>(r);
            const uchar* pg = warped.ptr<uchar>(r);
            for (int c = 0; c < cols; c++)
                if (pw[c]) { sum += pg[c]; n++; }
        }

        if (n <= 0)
        {
            nValid = 0;
            tm = cv::Mat(rows, cols, CV_32F, cv::Scalar(0.f));
            mk = cv::Mat(rows, cols, CV_32F, cv::Scalar(0.f));
            st2 = 0;
            if (spanOk) *spanOk = false;
            return;
        }

        const double mu = sum / (double)n;

        tm.create(rows, cols, CV_32F);
        mk.create(rows, cols, CV_32F);

        const bool wantSpan = (span0 && span1 && spanOk);
        if (wantSpan) { span0->assign((size_t)rows, -1); span1->assign((size_t)rows, -1); }
        bool contiguous = true;

        double s2 = 0;
        for (int r = 0; r < rows; r++)
        {
            const uchar* pw = wm.ptr<uchar>(r);
            const uchar* pg = warped.ptr<uchar>(r);
            float* pt = tm.ptr<float>(r);
            float* pm = mk.ptr<float>(r);

            int a = -1, b = -1;
            for (int c = 0; c < cols; c++)
            {
                if (!pw[c]) { pt[c] = 0.f; pm[c] = 0.f; continue; }

                const float v = (float)((double)pg[c] - mu);
                pt[c] = v;
                pm[c] = 1.f;
                s2 += (double)v * v;

                if (wantSpan)
                {
                    if (a < 0) a = c;
                    else if (contiguous && c != b + 1) contiguous = false;
                    b = c;
                }
            }
            if (wantSpan) { (*span0)[(size_t)r] = a; (*span1)[(size_t)r] = b; }
        }

        nValid = n;
        st2    = s2;
        if (spanOk) *spanOk = contiguous;
    }
}


//=======================================================================
//  ctor / dtor
//=======================================================================
NccMatcher::NccMatcher() : impl_(new Impl()) {}
NccMatcher::~NccMatcher() { delete impl_; }

//=======================================================================
//  create_ncc_model
//=======================================================================
bool NccMatcher::createNccModel(const cv::Mat& gray, const NccParams& p)
{
    created_ = false;
    // a reference point belongs to ONE model (its template size/centre), so it
    // must not leak into the next one
    resetOrigin();
    if (gray.empty()) return false;

    cv::Mat g8;
    if (gray.channels() == 3) cv::cvtColor(gray, g8, cv::COLOR_BGR2GRAY);
    else if (gray.channels() == 4) cv::cvtColor(gray, g8, cv::COLOR_BGRA2GRAY);
    else gray.convertTo(g8, CV_8U);

    params_ = p;

    //---- mask (optional) ----
    cv::Mat m0;
    if (!p.mask.empty())
    {
        cv::Mat m1;
        if (p.mask.channels() == 3) cv::cvtColor(p.mask, m1, cv::COLOR_BGR2GRAY);
        else p.mask.convertTo(m1, CV_8U);
        if (m1.size() != g8.size())
            cv::resize(m1, m1, g8.size(), 0, 0, cv::INTER_NEAREST);
        cv::threshold(m1, m0, 0, 255, cv::THRESH_BINARY);
    }

    //---- pyramid ----
    std::vector<cv::Mat> pir, mip;
    pir.push_back(g8);
    if (!m0.empty()) mip.push_back(m0);

    // Smallest template side that still gets its own pyramid level.
    //
    // 9 reproduces Halcon 17's own cap.  `get_ncc_model_params` shows Halcon
    // ignores a larger NumLevels request whenever the next level would be
    // smaller than ~9 px, and its coarsest patches are: 140x140 -> 5 levels
    // (9x9), 154x148 -> 5 (10x10), 110x110 -> 4 (14x14), 231x233 -> 5 (15x15)
    // - i.e. never 8x8.  With 8 we allowed a 6th level for 231x233 (8x8): an
    // 8x8 NCC over a PCB pad array has no position discrimination left, a wrong
    // pose scored 0.9179 there while the true pose scores 1.0000 at 5 levels,
    // so the search refined the wrong pose and reported 0.789 340 px away
    // (Halcon on the same template / image: 1.0000).  9 fixes that without
    // costing any levels on the 140x140 / 154x148 / 110x110 templates.
    const int minSide = 9;
    while (true)
    {
        if (p.numLevels > 0 && (int)pir.size() >= p.numLevels) break;
        cv::Mat next;
        cv::pyrDown(pir.back(), next);
        if (next.cols < minSide || next.rows < minSide) break;
        pir.push_back(next);
        if (!m0.empty())
        {
            cv::Mat nm;
            cv::pyrDown(mip.back(), nm);
            mip.push_back(nm);
        }
        if ((int)pir.size() > 7) break;
    }
    const int nLev = (int)pir.size();

    //---- angle grid ----
    // Halcon's AngleStep default is 1 degree for an 8-bit NCC model; the grid
    // halves per pyramid level exactly like the shape matcher does, so the
    // coarse level scans every 2^level degrees.
    const double aStep = (p.angleStep > 1e-9) ? p.angleStep : (kPi / 180.0);
    const double a0 = p.angleStart;
    const double aE = p.angleExtent;

    Impl& im = *impl_;
    im.levels.clear();
    im.gray0 = g8.clone();
    im.angleStep0 = aStep;
    im.levels.resize(nLev);

    for (int l = 0; l < nLev; l++)
    {
        LevelData& ld = im.levels[l];
        ld.gray = pir[l];
        if (!m0.empty()) ld.mask = mip[l];

        ld.srcCentre = cv::Point2f((ld.gray.cols - 1) * 0.5f,
                                   (ld.gray.rows - 1) * 0.5f);
        ld.canvas   = canvasForAngleRange(ld.gray.cols, ld.gray.rows, a0, aE);
        ld.centre   = cv::Point2f((ld.canvas.width - 1) * 0.5f,
                                  (ld.canvas.height - 1) * 0.5f);
        ld.n        = ld.canvas.width * ld.canvas.height;

        ld.usedPixels = ld.mask.empty() ? (ld.gray.cols * ld.gray.rows)
                                        : cv::countNonZero(ld.mask);

        // angle grid of this level (step doubles, so the count halves)
        const double stride = std::pow(2.0, (double)l);
        const double step   = aStep * stride;
        ld.angleStep = step;
        int nA = (int)std::floor(aE / step + 1e-9) + 1;
        if (nA < 1) nA = 1;
        // a full circle must not repeat 0 and 360
        if (aE >= 2 * kPi - 1e-6 && nA > 1)
        {
            const int full = (int)std::lround(2 * kPi / step);
            if (full > 1) nA = full;
        }
        ld.angles.resize(nA);
        for (int i = 0; i < nA; i++) ld.angles[i] = a0 + i * step;
    }

    // A new model invalidates every cached rotation.  Size the LRU to hold the
    // union of all levels' angle grids, so a steady stream on ONE model never
    // overflows (the clear-on-overflow path only matters if the same matcher is
    // repointed at wildly different angle ranges without re-creating the model).
    size_t totalAng = 0;
    for (const LevelData& ld : impl_->levels) totalAng += ld.angles.size();
    impl_->rotCacheLimit = std::max<size_t>(64, totalAng);
    impl_->rotCache.clear();
    impl_->rotCacheHits = impl_->rotCacheMiss = 0;

    created_ = true;
    return true;
}

namespace
{
    inline double msSince(int64 t0)
    {
        return (double)(cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();
    }

    //---------------------------------------------------------------------
    //  Per-level preparation: only the size of the score map is left here.
    //
    //  SI and SI2 used to come from integral images over the canvas RECTANGLE,
    //  which is only valid while the scored region is a rectangle.  It is not:
    //  the region is the rotated template (see buildRotTemplate), so SI / SI2 are
    //  two filter2D with that region as the kernel, computed per angle in
    //  scoreMapFor.  Cost is unchanged - those convolutions were 2 of the 3 per
    //  angle before - and the two integral images per level disappear.
    //---------------------------------------------------------------------
    bool prepareLevel(LevelData& ld, const cv::Mat& img8)
    {
        ld.mapW = img8.cols - ld.canvas.width + 1;
        ld.mapH = img8.rows - ld.canvas.height + 1;
        return ld.mapW > 0 && ld.mapH > 0;
    }

    //---------------------------------------------------------------------
    //  NCC score map for one angle.
    //
    //  filter2D computes  dst(x,y) = sum_{u,v} K(u,v) * src(x+u-ax, y+v-ay),
    //  i.e. correlation, so with the anchor at the canvas centre and K = Tm it is
    //  exactly  SIT(x,y) = sum_k Tm_k * I(x + ox_k, y + oy_k).
    //
    //  SI and SI2 use the rotated template region as the kernel instead of a
    //  rectangle, so only the valid part of the canvas enters the normalisation.
    //  Both are produced by ONE filter2D on the 2-channel image [I, I^2] that
    //  findNccModel builds once per level (`two`), which is why this is two
    //  convolutions per pose rather than three.
    //---------------------------------------------------------------------
    void scoreMapFor(const LevelData& ld, const cv::Mat& fimg, const cv::Mat& two,
                     double angle, cv::Mat& out)
    {
        cv::Mat tm, mk;
        double st2 = 0;
        int nValid = 0;
        buildRotTemplate(ld, angle, tm, mk, st2, nValid);

        if (nValid <= 0 || st2 <= 1e-9)
        {
            out = cv::Mat(ld.mapH, ld.mapW, CV_32F, cv::Scalar(0.f));
            return;
        }

        const cv::Point anchor((int)ld.centre.x, (int)ld.centre.y);
        const int ax = anchor.x, ay = anchor.y;

        // [I, I^2] -> one convolution gives SI and SI2 together
        cv::Mat sums;
        cv::filter2D(two, sums, CV_32F, mk, anchor, 0.0, cv::BORDER_CONSTANT);
        std::vector<cv::Mat> ch;
        cv::split(sums, ch);
        const cv::Mat cropI  = ch[0](cv::Rect(ax, ay, ld.mapW, ld.mapH));
        const cv::Mat cropI2 = ch[1](cv::Rect(ax, ay, ld.mapW, ld.mapH));

        cv::Mat sit;
        cv::filter2D(fimg, sit, CV_32F, tm, anchor, 0.0, cv::BORDER_CONSTANT);
        const cv::Mat cropS = sit(cv::Rect(ax, ay, ld.mapW, ld.mapH));

        const double n = (double)nValid;
        const double denT = std::sqrt(n * st2);

        out.create(ld.mapH, ld.mapW, CV_32F);
        for (int y = 0; y < ld.mapH; y++)
        {
            const float* ps = cropS.ptr<float>(y);
            const float* p1 = cropI.ptr<float>(y);
            const float* p2 = cropI2.ptr<float>(y);
            float* po = out.ptr<float>(y);
            for (int x = 0; x < ld.mapW; x++)
            {
                const double si = p1[x], si2 = p2[x];
                const double varI = n * si2 - si * si;
                if (varI <= 1e-6) { po[x] = 0.f; continue; }   // flat image patch
                po[x] = (float)(n * (double)ps[x] / (std::sqrt(varI) * denT));
            }
        }
    }

    //---------------------------------------------------------------------
    //  NCC at one integer canvas position, for an already rotated template.
    //  Same formula as scoreAtPose, but the rotation work is done by the caller
    //  so it can be reused for several positions / be amortised over the whole
    //  level-0 refinement (see the note in findNccModel).
    //---------------------------------------------------------------------
    double scoreAtPrepared(const LevelData& ld, const cv::Mat& img8, const cv::Mat& fimg,
                           const cv::Mat& tm, const cv::Mat& mk,
                           double st2, int nValid, cv::Point2f centre)
    {
        if (nValid <= 0 || st2 <= 1e-9) return 0.0;

        const int cw = ld.canvas.width, ch = ld.canvas.height;
        const int x0 = cvRound(centre.x - ld.centre.x);
        const int y0 = cvRound(centre.y - ld.centre.y);
        if (x0 < 0 || y0 < 0 || x0 + cw > img8.cols || y0 + ch > img8.rows) return 0.0;

        // SIT and SI / SI2 over the valid region only.
        //
        // Keep the `continue` branch: a branch-free version that multiplies SI /
        // SI2 by the 0/1 mask instead was measured SLOWER (refine 134 -> 193 ms
        // on 154x148 / 5 levels) because MSVC does not vectorise the float->double
        // chain, so the invalid half of the canvas is computed for nothing.
        double sit = 0, si = 0, si2 = 0;
        for (int r = 0; r < ch; r++)
        {
            const float* pt = tm.ptr<float>(r);
            const float* pm = mk.ptr<float>(r);
            const float* pi = fimg.ptr<float>(y0 + r) + x0;
            const uchar* pg = img8.ptr<uchar>(y0 + r) + x0;
            double s0 = 0, s1 = 0, s2 = 0;
            for (int c = 0; c < cw; c++)
            {
                if (pm[c] < 0.5f) continue;
                s0 += (double)pt[c] * pi[c];
                s1 += (double)pg[c];
                s2 += (double)pg[c] * pg[c];
            }
            sit += s0; si += s1; si2 += s2;
        }

        const double n = (double)nValid;
        const double varI = n * si2 - si * si;
        if (varI <= 1e-6) return 0.0;
        return n * sit / (std::sqrt(varI) * std::sqrt(n * st2));
    }

    //---------------------------------------------------------------------
    //  NCC at one exact pose.  Same formula, but the sums are plain dot products
    //  over the canvas instead of a convolution, so it costs O(canvas) rather
    //  than a full filter2D.  Used by the level-0 refinement, the sub-pixel fits
    //  and probe().
    //---------------------------------------------------------------------
    double scoreAtPose(const LevelData& ld, const cv::Mat& img8, const cv::Mat& fimg,
                       cv::Point2f centre, double angle)
    {
        cv::Mat tm, mk;
        double st2 = 0;
        int nValid = 0;
        buildRotTemplate(ld, angle, tm, mk, st2, nValid);
        return scoreAtPrepared(ld, img8, fimg, tm, mk, st2, nValid, centre);
    }
}

namespace
{
    // Halcon's Metric for NCC: 'ignore_global_polarity' means a fully inverted
    // template still matches, and since NCC(-T) == -NCC(T) that is just |NCC|.
    inline void applyMetric(cv::Mat& m, int metric)
    {
        if (metric == 1) cv::absdiff(m, cv::Scalar::all(0), m);
    }

    struct Cand
    {
        cv::Point2f pos;
        double      angle = 0;
        double      score = -2;
    };

    // keep the strongest candidates, dropping ones that sit on the same pose
    void dedupe(std::vector<Cand>& in, std::vector<Cand>& out, int keep,
                double posTol, double angTol)
    {
        std::sort(in.begin(), in.end(),
                  [](const Cand& a, const Cand& b) { return a.score > b.score; });
        out.clear();
        for (size_t i = 0; i < in.size(); i++)
        {
            const Cand& c = in[i];
            bool dup = false;
            for (size_t j = 0; j < out.size(); j++)
            {
                const Cand& o = out[j];
                if (std::fabs(c.pos.x - o.pos.x) <= posTol &&
                    std::fabs(c.pos.y - o.pos.y) <= posTol &&
                    angleDiff(c.angle, o.angle) <= angTol)
                {
                    dup = true; break;
                }
            }
            if (!dup) out.push_back(c);
            if ((int)out.size() >= keep) break;
        }
    }

    //---------------------------------------------------------------------
    //  Per-row valid column interval of the rotated mask.
    //
    //  For a 360-degree model the canvas is padded out to the template
    //  diagonal, so half of it is empty.  Walking the mask row by row turns
    //  "every pixel plus a branch" into "one contiguous run plus no branch",
    //  which is exactly what the level-0 refinement (the most expensive stage)
    //  needs.  A user-painted mask can have holes, so the interval is only
    //  reported when EVERY row is contiguous; otherwise the caller falls back
    //  to the branching version and the result is bit-identical either way.
    //---------------------------------------------------------------------
    bool buildRowSpan(const cv::Mat& mk, std::vector<int>& c0, std::vector<int>& c1)
    {
        const int rows = mk.rows, cols = mk.cols;
        c0.assign(rows, -1);
        c1.assign(rows, -1);
        for (int r = 0; r < rows; r++)
        {
            const float* pm = mk.ptr<float>(r);
            int a = -1, b = -1;
            for (int c = 0; c < cols; c++)
            {
                if (pm[c] < 0.5f) continue;
                if (a < 0) { a = c; }
                else if (c != b + 1) { return false; }  // hole -> not contiguous
                b = c;
            }
            c0[r] = a;
            c1[r] = b;
        }
        return true;
    }

    // Pack (level, angle index) into the LRU key.  Angle index fits in 32 bits
    // even for a 360-degree / 0.25-degree model, level in the high word.
    inline uint64_t rotKey(int level, int angleIdx)
    {
        return (uint64_t)(uint32_t)level << 32 | (uint32_t)angleIdx;
    }

    // Return the rotated template for (level, angleIdx), building it on a miss.
    // SERIAL ONLY: the cache is a plain member map with no lock, so this must be
    // called from the serial pre-build section, never from a parallel_for_ body.
    // On overflow the whole cache is dropped (cheaper and safer than evicting an
    // entry a concurrent reader might still hold a pointer to).
    RotTpl& getRotTpl(std::unordered_map<uint64_t, RotTpl>& cache, size_t limit,
                      int& nHits, int& nMiss,
                      const LevelData& ld, int level, int angleIdx)
    {
        const uint64_t key = rotKey(level, angleIdx);
        auto it = cache.find(key);
        if (it != cache.end())
        {
            nHits++;
            return it->second;
        }

        nMiss++;
        if (cache.size() >= limit)
            cache.clear();

        RotTpl rt;
        buildRotTemplate(ld, ld.angles[angleIdx], rt.tm, rt.mk,
                         rt.st2, rt.nValid, &rt.c0, &rt.c1, &rt.spanOk);
        rt.ready = true;
        auto res = cache.emplace(key, std::move(rt));
        return res.first->second;
    }

    //---------------------------------------------------------------------
    //  Same score as scoreAtPrepared(), but walking only each row's valid
    //  interval (see buildRowSpan).  Identical formula, identical sums, so the
    //  numbers are bit-for-bit what scoreAtPrepared returns - verified with
    //  ncc_selfcheck (1.000000 peaks) and case1 (identical scores/poses).
    //---------------------------------------------------------------------
    double scoreAtSpan(const LevelData& ld, const cv::Mat& img8, const cv::Mat& fimg,
                       const cv::Mat& tm, const std::vector<int>& c0,
                       const std::vector<int>& c1, double st2, int nValid,
                       cv::Point2f centre)
    {
        if (nValid <= 0 || st2 <= 1e-9) return 0.0;

        const int cw = ld.canvas.width, ch = ld.canvas.height;
        const int x0 = cvRound(centre.x - ld.centre.x);
        const int y0 = cvRound(centre.y - ld.centre.y);
        if (x0 < 0 || y0 < 0 || x0 + cw > img8.cols || y0 + ch > img8.rows) return 0.0;

        double sit = 0, si = 0, si2 = 0;
        for (int r = 0; r < ch; r++)
        {
            const int a = c0[r], b = c1[r];
            if (a < 0) continue;                        // empty row
            const float* pt = tm.ptr<float>(r) + a;
            const float* pi = fimg.ptr<float>(y0 + r) + x0 + a;
            const uchar* pg = img8.ptr<uchar>(y0 + r) + x0 + a;
            const int n = b - a + 1;
            double s0 = 0, s1 = 0, s2 = 0;
            for (int c = 0; c < n; c++)
            {
                s0 += (double)pt[c] * pi[c];
                s1 += (double)pg[c];
                s2 += (double)pg[c] * pg[c];
            }
            sit += s0; si += s1; si2 += s2;
        }

        const double n = (double)nValid;
        const double varI = n * si2 - si * si;
        if (varI <= 1e-6) return 0.0;
        return n * sit / (std::sqrt(varI) * std::sqrt(n * st2));
    }

    cv::Mat toGray8(const cv::Mat& src)
    {
        cv::Mat g8;
        if (src.channels() == 3) cv::cvtColor(src, g8, cv::COLOR_BGR2GRAY);
        else if (src.channels() == 4) cv::cvtColor(src, g8, cv::COLOR_BGRA2GRAY);
        else src.convertTo(g8, CV_8U);
        return g8;
    }

    // overlap of two rotated template rectangles, used by MaxOverlap
    double rectOverlap(const cv::Size2f& tsz, const NccResult& a, const NccResult& b)
    {
        if (tsz.width <= 0.f || tsz.height <= 0.f) return 0.0;
        // the pose maps the model with [[cos, sin], [-sin, cos]], i.e. a rotation
        // of -angle; RotatedRect counts the angle the other way round (y down)
        cv::RotatedRect ra(a.pos, tsz, -a.angle * 180.0 / kPi);
        cv::RotatedRect rb(b.pos, tsz, -b.angle * 180.0 / kPi);
        std::vector<cv::Point2f> inter;
        const int rc = cv::rotatedRectangleIntersection(ra, rb, inter);
        const double area = (double)tsz.width * (double)tsz.height;
        if (rc == cv::INTERSECT_NONE || inter.size() < 3 || area <= 0.0) return 0.0;
        return std::min(1.0, std::fabs(cv::contourArea(inter)) / area);
    }
}

//=======================================================================
//  find_ncc_model
//=======================================================================
std::vector<NccResult> NccMatcher::findNccModel(const cv::Mat& gray, const NccFindParams& fp) const
{
    std::vector<NccResult> results;
    if (!created_ || gray.empty()) return results;

    Impl& im = *impl_;
    const int nModelLev = (int)im.levels.size();
    if (nModelLev < 1) return results;

    int maxLev = nModelLev;
    if (fp.numLevels > 0) maxLev = std::min(maxLev, fp.numLevels);
    const int top = maxLev - 1;

    const cv::Mat g8 = toGray8(gray);

    //---- source pyramid ----
    const int64 t0 = cv::getTickCount();
    std::vector<cv::Mat> py(top + 1), fpy(top + 1);
    py[0] = g8;
    for (int i = 1; i <= top; i++) cv::pyrDown(py[i - 1], py[i]);
    for (int i = 0; i <= top; i++) py[i].convertTo(fpy[i], CV_32F);

    // [I, I^2] for the coarse scan: one convolution then yields SI and SI2 at
    // every position.  The refinement does not need it - it scores only the
    // positions around each candidate (see the note further down).
    // Built inside the coarse loop below, because a fallback to a finer level
    // needs the [I, I^2] of THAT level.

    std::vector<int> usable(top + 1, 0);
    for (int l = 0; l <= top; l++) usable[l] = prepareLevel(im.levels[l], py[l]) ? 1 : 0;
    im.lastPre = msSince(t0);

    const int64 t1 = cv::getTickCount();
    double tCoarse = 0, tRefine = 0, tSub = 0;      // per-stage split, for fp.debug

    //---- coarse scan, with a fallback to a finer level ----
    //
    // The coarsest level can be unusable without looking broken: for a 154x148
    // template the 5th level is a 9x9 patch, and on a texture-only scene its
    // score map peaks around 0.55 on random noise while the real pose is nowhere
    // in the candidate list.  Halcon's manual warns about the same thing ("the
    // automatically determined number of levels can be too high").  So if the
    // coarse peak is not clearly above the acceptance threshold, the scan is
    // repeated one level finer and the whole search continues from there.
    //
    // Threshold: a correct pose scores ~0.80 at the coarsest level on the real
    // images and ~0.55 on pure noise, so anything below ~0.6 means "no signal".
    std::vector<Cand> cands;
    int coarseTop = top;
    // Peak of the previous (one-level-coarser) scan, -2 = none yet.  Used by the
    // "does falling back actually help?" test further down.
    double prevCoarsePeak = -2.0;
    {
        const double trust = std::max(0.7, std::min(0.9, fp.minScore * 1.2));

        // A fallback to a finer level only makes sense when the finer level sees
        // a STRONGER peak: that is what "the coarsest level blurred the target
        // away" looks like.  The minimum improvement is this absolute margin
        // (0.02 by default).
        //
        // SM_NCC_FALLBACK_GAIN=<0..1> overrides the margin; a NEGATIVE value
        // disables the guard entirely (= the old "always fall back" behaviour),
        // so the two can be A/B compared on the same images.
        double fallbackGain = 0.02;
        if (const char* gv = std::getenv("SM_NCC_FALLBACK_GAIN"))
        {
            const double v = std::atof(gv);
            if (v >= -1.0 && v <= 1.0) fallbackGain = v;
        }

        // Never fall back further than two levels: level 0 means a full-resolution
        // score map for every angle (seconds per image), which would turn a
        // "nothing here" image into a stall.  Two levels down is the 38x37
        // template for a 154x148 model - the level the old 3-level default used.
        const int coarseFloor = std::max(1, top - 2);

        for (int ct = top; ct >= coarseFloor; ct--)
        {
            coarseTop = ct;
            if (!usable[ct]) { continue; }

            cv::Mat two, sqCt;
            cv::multiply(fpy[ct], fpy[ct], sqCt);
            cv::merge(std::vector<cv::Mat>{ fpy[ct], sqCt }, two);

            const LevelData& ld = im.levels[ct];

        const int64 tCoarseLv = cv::getTickCount();
        const double target = fp.minScore * 0.6;   // the coarse level is blurry
        // `all` is pruned to the globally strongest few thousand, so a texture
        // rich image cannot blow up memory - but the strongest peaks are always
        // kept, which is what the old per-map cap got wrong.
        const size_t maxCands = 20000;

        // Angles are independent, so each one is a parallel task.  Every task
        // writes only its own slot (a local candidate list and its own map max),
        // the maps are then merged in angle order - so the result is the same as
        // the serial loop, just spread over the cores.  A texture-only scene can
        // produce thousands of local maxima per angle, which is why the merge
        // keeps the global prune that the old loop did after every angle.
        struct AngleOut
        {
            std::vector<Cand> all;
            double      maxVal = -2;
            cv::Point   maxPos{ 0, 0 };
        };
        const int nAng = (int)ld.angles.size();
        std::vector<AngleOut> outs((size_t)nAng);

        cv::parallel_for_(cv::Range(0, nAng), [&](const cv::Range& rng)
        {
            cv::Mat map;
            for (int ai = rng.start; ai < rng.end; ai++)
            {
                AngleOut& ao = outs[(size_t)ai];
                scoreMapFor(ld, fpy[ct], two, ld.angles[ai], map);
                applyMetric(map, params_.metric);
                if (map.empty()) continue;

                double mn, mx;
                cv::Point mnl, mxl;
                cv::minMaxLoc(map, &mn, &mx, &mnl, &mxl);
                ao.maxVal = mx;
                ao.maxPos = mxl;

                // Every local maximum above the target goes into the list.  Do
                // NOT cap per map while scanning: the scan goes top to bottom, so
                // a cap silently drops the real peak as soon as the rows above it
                // are busy with texture (that is exactly how a 0.9756 peak was
                // lost to 32 x 0.2 spurious maxima at 1280x1024).
                for (int y = 0; y < map.rows; y++)
                {
                    const float* pc = map.ptr<float>(y);
                    const float* pm = map.ptr<float>(std::max(0, y - 1));
                    const float* pp = map.ptr<float>(std::min(map.rows - 1, y + 1));
                    for (int x = 0; x < map.cols; x++)
                    {
                        const float v = pc[x];
                        if (v < (float)target) continue;
                        const int xm = std::max(0, x - 1), xp = std::min(map.cols - 1, x + 1);
                        float m = std::max(pc[xm], pc[xp]);
                        m = std::max(m, std::max(pm[xm], std::max(pm[x], pm[xp])));
                        m = std::max(m, std::max(pp[xm], std::max(pp[x], pp[xp])));
                        if (v < m - 1e-5f) continue;             // not a local maximum

                        Cand c;
                        c.pos   = cv::Point2f(x + ld.centre.x, y + ld.centre.y);
                        c.angle = ld.angles[(size_t)ai];
                        c.score = v;
                        ao.all.push_back(c);
                    }
                }
            }
        });

        std::vector<Cand> all;
        double bestMapVal = -2;
        cv::Point bestMapPos(0, 0);
        double bestMapAng = 0;
        for (int ai = 0; ai < nAng; ai++)
        {
            const AngleOut& ao = outs[(size_t)ai];
            if (ao.maxVal > bestMapVal)
            {
                bestMapVal = ao.maxVal;
                bestMapPos = ao.maxPos;
                bestMapAng = ld.angles[(size_t)ai];
            }
            all.insert(all.end(), ao.all.begin(), ao.all.end());
        }
        if (all.size() > maxCands)
        {
            std::nth_element(all.begin(), all.begin() + maxCands, all.end(),
                             [](const Cand& a, const Cand& b) { return a.score > b.score; });
            all.resize(maxCands);
        }

        const int keep = std::max(fp.maxMatches * 4, 16);
        dedupe(all, cands, keep, 2.0 * (double)(1 << ct), ld.angleStep * 1.01);

        if (fp.debug >= 2)
        {
            std::printf("[ncc]   level %d (coarse): map max %.4f at centre (%.1f,%.1f) angle %.2f\n",
                        ct, bestMapVal,
                        bestMapPos.x + im.levels[ct].centre.x,
                        bestMapPos.y + im.levels[ct].centre.y,
                        bestMapAng * 180.0 / CV_PI);
            std::printf("[ncc]   level %d (coarse): canv %dx%d  %d angles  "
                        "%d local maxima -> %d candidates  %.2f ms\n",
                        ct, ld.canvas.width, ld.canvas.height, nAng,
                        (int)all.size(), (int)cands.size(), msSince(tCoarseLv));
            for (size_t i = 0; i < cands.size() && i < 10; i++)
                std::printf("[ncc]     %2d  score %.4f  pos (%7.2f,%7.2f)  angle %7.2f\n",
                            (int)i, cands[i].score, cands[i].pos.x, cands[i].pos.y,
                            cands[i].angle * 180.0 / CV_PI);
        }

        if (bestMapVal >= trust || ct == coarseFloor) break;

        // A fallback is only worth its price when the FINER level sees a
        // stronger peak; that is exactly what "the coarsest level blurred the
        // target away" looks like.  When the peak stays flat or drops, no target
        // is there and every further fallback just repeats a full-image scan at
        // double resolution.
        //
        // Measured on 2048x2448 (no target, 360 degrees, minScore 0.30): peaks
        // 0.4738 -> 0.4161 -> 0.3844, i.e. monotonically falling, and 87% of the
        // 2.6 s search time was the SECOND fallback (73x73 canvas x 180 angles).
        // Real targets do the opposite: the finer level resolves them and the
        // peak rises well above this margin.
        if (fallbackGain >= 0.0 && prevCoarsePeak > -2.0 &&
            bestMapVal < prevCoarsePeak + fallbackGain)
        {
            if (fp.debug >= 1)
                std::printf("[ncc] coarse peak %.4f at level %d does not improve on "
                            "level %d (%.4f): keeping the candidates found here\n",
                            bestMapVal, ct, ct + 1, prevCoarsePeak);
            break;
        }
        prevCoarsePeak = bestMapVal;

        if (fp.debug >= 1)
            std::printf("[ncc] coarse peak %.4f at level %d is below %.2f, "
                        "rescanning one level finer\n", bestMapVal, ct, trust);
        cands.clear();
        }
    }

    tCoarse = msSince(t1);

    //---- refine, one level down at a time (starting below the level that was
    //     actually scanned, which may be finer than top after a fallback) ----
    for (int l = coarseTop - 1; l >= 0; l--)
    {
        const LevelData& ld = im.levels[l];
        if (!usable[l] || cands.empty()) continue;

        const int nA = (int)ld.angles.size();
        if (nA <= 0) continue;

        // which angle indices does every candidate need (+-1 step of this level)?
        // Angle search window during refinement.
        //
        // A coarse level only resolves the angle to one of ITS steps, so the true
        // angle can sit up to +-1 coarse step away; refining with +-1 step per
        // level then just barely covers it and any extra coarse error is never
        // recovered.  With the deep pyramid (5 levels, coarsest template 9x9,
        // step 16 deg) that showed up as a synthetic 40x30 / 360-degree case
        // scoring 0.951357 at -0.16 deg instead of 1.000000 at 0 deg: the coarse
        // angle was one grid cell off and the refinement could not undo it.
        // +-2 steps fixes it; the clamp keeps short grids (nA <= 4) sane.
        // Every refinement level needs it, not just the first one: a refinement
        // that picks the best of +-1 steps leaves up to one step of error, and one
        // step at level l is two steps at level l-1, so +-1 never converges (measured:
        // restricting the wide window to the first level put the 40x30 / 360 case
        // straight back to 0.951357).
        const int angWin = std::min(2, (nA - 1) / 2);
        std::vector<std::vector<int>> candAngles(cands.size());
        for (size_t ci = 0; ci < cands.size(); ci++)
        {
            const int base = (int)std::lround((cands[ci].angle - ld.angles[0]) / ld.angleStep);
            for (int d = -angWin; d <= angWin; d++)
            {
                int idx = base + d;
                if (idx < 0) idx += nA;
                if (idx >= nA) idx -= nA;
                if (idx < 0 || idx >= nA) continue;
                candAngles[ci].push_back(idx);
            }
        }

        // A full score map is one convolution of the WHOLE image per angle
        // (measured ~110 ms at level 0 for a 355x414 template on a 1280x1024
        // image), while the refiner only ever reads a 5x5 window around each
        // candidate.  So it evaluates the score at exactly the positions it
        // needs, with the template rotated once per angle - the same numbers as
        // the map, for a fraction of the work.
        //
        // The rotated templates are resolved HERE, serially, for every angle
        // index a candidate needs: the loop below runs in parallel and must only
        // READ them.  getRotTpl serves each (level, angle) from the cross-call LRU
        // (a video stream reuses every entry after the first frame) and only builds
        // on a miss, so the warpAffine work - the dominant refine cost - is paid once
        // per angle for the whole stream rather than once per image.
        //
        // We hold raw pointers, not Mat copies: the parallel section must not mutate
        // the cache (no lock), and pointers are re-resolved every call, so a cache
        // clear on an overflow can never leave a dangling reference across frames.
        std::vector<const RotTpl*> rot(nA, nullptr);
        const int64 tRot = cv::getTickCount();
        {
            std::vector<char> need((size_t)nA, 0);
            for (size_t ci = 0; ci < candAngles.size(); ci++)
                for (size_t k = 0; k < candAngles[ci].size(); k++)
                    need[(size_t)candAngles[ci][k]] = 1;

            for (int idx = 0; idx < nA; idx++)
            {
                if (!need[(size_t)idx]) continue;
                rot[(size_t)idx] = &getRotTpl(im.rotCache, im.rotCacheLimit,
                                               im.rotCacheHits, im.rotCacheMiss,
                                               ld, l, idx);
            }
        }
        const double msRot = msSince(tRot);
        int nRotBuilt = 0;
        for (int i = 0; i < nA; i++) if (rot[(size_t)i] && rot[(size_t)i]->ready) nRotBuilt++;
        const int64 tScore = cv::getTickCount();

        // Position mapping from the coarse level to this one.
        //
        // It must NOT be a hard coded *2: pyrDown floors odd sizes, so a 700x700
        // image gives 700 -> 350 -> 175 -> 87 -> 43, and 175/87 /= 2.  A 0.5 px
        // error at the coarsest level is multiplied by 8..16 on the way down and
        // pushes the candidate out of the +-2 refinement box (measured: the
        // rotated-ground-truth scene went from 7/7 hits to 0/7 that way, while
        // 1280x1024 images - all exact powers of two - kept working).
        // Using the real size ratio is identical to *2 whenever the sizes do halve.
        // The ratio MUST come from the SEARCH image pyramid (py), not from the
        // template pyramid (im.levels): cands[].pos is a coordinate in the search
        // image, and the template's own halving (154 -> 77 -> 38 -> 19) is coarse
        // (77/38 = 2.026), so using it shifts the candidate by pixels every level.
        // That was the actual reason a 4-level model lost every case1 match while
        // its coarse peak (0.8931) was already the correct pose.
        const double rx = (double)py[l].cols / (double)std::max(1, py[l + 1].cols);
        const double ry = (double)py[l].rows / (double)std::max(1, py[l + 1].rows);

        // Parallelise over (candidate, angle) pairs rather than over candidates:
        // at level 0 there are only 4 candidates but 5 angles each, so one task
        // per candidate leaves most of the 20 cores idle.  Each task scores its
        // own 5x5 window and writes only its own slot, and the per-candidate
        // winner is then picked in the same angle order as the serial loop, so
        // every score is computed exactly as before.
        struct AngTask { size_t ci; int idx; };
        std::vector<AngTask> tasks;
        tasks.reserve(cands.size() * 5);
        for (size_t ci = 0; ci < cands.size(); ci++)
            for (size_t k = 0; k < candAngles[ci].size(); k++)
                tasks.push_back({ ci, candAngles[ci][k] });

        std::vector<cv::Point2f> mapped(cands.size());
        for (size_t ci = 0; ci < cands.size(); ci++)
            mapped[ci] = cv::Point2f(cands[ci].pos.x * (float)rx,
                                     cands[ci].pos.y * (float)ry);   // one level down

        struct TaskBest
        {
            double      score = -2;
            cv::Point2f pos{ 0.f, 0.f };
        };
        std::vector<TaskBest> tb(tasks.size());

        // Position window: +-3 on the FIRST refinement (coarsest -> one level
        // down) and +-2 afterwards.
        //
        // The coarse level only locates the peak on its own integer grid, so the
        // mapped position can be off by ~2 px at the next level up.  +-1 was
        // measured to clip the peak on the real case1 images (0.897 -> 0.868);
        // +-3 on the first level is what keeps a 4-level pyramid within the
        // accuracy of the old 3-level one (the 90-degree ground-truth pose
        // otherwise lands 3.2 px off at 0.6087).
        const int posWin = (l == coarseTop - 1) ? 3 : 2;

        cv::parallel_for_(cv::Range(0, (int)tasks.size()), [&](const cv::Range& rng)
        {
            for (int ti = rng.start; ti < rng.end; ti++)
            {
                const AngTask& t = tasks[(size_t)ti];
                const RotTpl* rt = rot[(size_t)t.idx];
                if (!rt || !rt->ready) continue;

                const int cx = cvRound(mapped[t.ci].x - ld.centre.x);
                const int cy = cvRound(mapped[t.ci].y - ld.centre.y);

                TaskBest best;
                for (int dy = -posWin; dy <= posWin; dy++)
                {
                    const int y = cy + dy;
                    if (y < 0 || y >= ld.mapH) continue;
                    for (int dx = -posWin; dx <= posWin; dx++)
                    {
                        const int x = cx + dx;
                        if (x < 0 || x >= ld.mapW) continue;
                        const cv::Point2f p(x + ld.centre.x, y + ld.centre.y);
                        double v = rt->spanOk
                            ? scoreAtSpan(ld, py[l], fpy[l], rt->tm, rt->c0, rt->c1,
                                          rt->st2, rt->nValid, p)
                            : scoreAtPrepared(ld, py[l], fpy[l], rt->tm, rt->mk,
                                              rt->st2, rt->nValid, p);
                        if (params_.metric == 1) v = std::fabs(v);
                        if (v > best.score) { best.score = v; best.pos = p; }
                    }
                }
                tb[(size_t)ti] = best;
            }
        });

        // serial merge: same angle order, so the winner per candidate is the one
        // the serial version would have picked
        std::vector<Cand> survivors;
        survivors.reserve(cands.size());
        size_t ti = 0;
        for (size_t ci = 0; ci < cands.size(); ci++)
        {
            Cand best;
            best.pos   = mapped[ci];
            best.angle = cands[ci].angle;
            best.score = -2;

            const size_t nAng = candAngles[ci].size();
            for (size_t k = 0; k < nAng; k++, ti++)
            {
                const TaskBest& b = tb[ti];
                if (b.score > best.score)
                {
                    best.score = b.score;
                    best.pos   = b.pos;
                    best.angle = ld.angles[(size_t)candAngles[ci][k]];
                }
            }
            if (best.score > -1.5) survivors.push_back(best);
        }

        // Prune the candidate list as the levels get finer.  Keeping the full
        // coarse list (30) at EVERY level is what made the level-0 pass dominate:
        // one candidate costs 3 angles x 25 positions x a 214x214 canvas dot
        // product, so 30 candidates at level 0 alone was ~100 M multiply-adds.
        const double msScore = msSince(tScore);

        // The coarse list stays wide (16), the finest levels only see survivors.
        // Halcon prunes the same way; a peak that is not among the strongest 10
        // after four pyramid levels will not win at level 0 either.
        const int keepNext = std::max(fp.maxMatches * 3, 8);
        // The level-0 pass is by far the most expensive: a 214x214 canvas for a
        // 154x148 360-degree model, and the rotated template has to be built for
        // every (candidate x angle) pair that survives - 15 of them with only 4
        // candidates, i.e. 6.8 ms of the 20 ms search.  So hand level 0 as few
        // candidates as possible; after four pyramid levels a peak that is still
        // ranked 3rd cannot overtake the top two (verified on case1/test1: all
        // images keep the same score and pose).
        int keepThis = keepNext;
        if (l == 2) keepThis = std::min(keepThis, std::max(fp.maxMatches * 3, 4));
        if (l == 1) keepThis = std::min(keepThis, std::max(fp.maxMatches * 2, 2));
        std::vector<Cand> pruned;
        dedupe(survivors, pruned, keepThis, 2.0, ld.angleStep * 1.01);
        cands.swap(pruned);

        if (fp.debug >= 2)
        {
            std::printf("[ncc]   level %d: %d cands -> %d kept   canv %dx%d   "
                        "%d rotTpl %.2f ms  score %.2f ms\n",
                        l, (int)survivors.size(), (int)cands.size(),
                        ld.canvas.width, ld.canvas.height, nRotBuilt, msRot, msScore);
            for (size_t i = 0; i < cands.size() && i < 5; i++)
                std::printf("[ncc]     %2d  score %.4f  pos (%7.2f,%7.2f)  angle %7.2f\n",
                            (int)i, cands[i].score, cands[i].pos.x, cands[i].pos.y,
                            cands[i].angle * 180.0 / CV_PI);
        }
    }

    tRefine = msSince(t1) - tCoarse;
    im.lastSearch = msSince(t1);

    //---- sub-pixel: Halcon SubPixel = 'interpolation', i.e. a parabola fitted to
    //     the score surface, separately for angle and for position ----
    if (fp.subpixel && !cands.empty() && usable[0])
    {
        const LevelData& ld = im.levels[0];
        const double da = ld.angleStep;

        // one parallel task per candidate: each one only reads ld / py / fpy and
        // writes its own cands[i], so no locking is needed
        cv::parallel_for_(cv::Range(0, (int)cands.size()), [&](const cv::Range& rng)
        {
            for (int ii = rng.start; ii < rng.end; ii++)
            {
                Cand& c = cands[(size_t)ii];

                // angle: three samples at the winning position (O(canvas) each)
                {
                    double s0 = c.score;
                    double sm = scoreAtPose(ld, py[0], fpy[0], c.pos, c.angle - da);
                    double sp = scoreAtPose(ld, py[0], fpy[0], c.pos, c.angle + da);
                    if (params_.metric == 1) { sm = std::fabs(sm); sp = std::fabs(sp); s0 = std::fabs(s0); }
                    const double den = 2.0 * s0 - sp - sm;
                    if (den > 1e-6)
                    {
                        double t = clampD(0.5 * (sp - sm) / den, -1.0, 1.0);
                        c.angle += t * da;
                    }
                }

                // position: parabola through the 3x3 scores around the winner.  A
                // score map is not worth it here either (see the note above); the
                // numbers are the same, position by position.
                //
                // All nine positions share the SAME angle, so the rotated template is
                // built once - buildRotTemplate (two warpAffine + mean + setTo + two
                // sums over the canvas) costs far more than the nine 5x5 dot products
                // it would otherwise be repeated for.  Measured on 154x148 / 360 deg:
                // 11 rotations per candidate -> 3, sub-pixel stage 138 ms -> ~40 ms.
                const int cx = cvRound(c.pos.x - ld.centre.x);
                const int cy = cvRound(c.pos.y - ld.centre.y);
                if (cx > 0 && cy > 0 && cx + 1 < ld.mapW && cy + 1 < ld.mapH)
                {
                    cv::Mat ptm, pmk;
                    double pst2 = 0;
                    int pValid = 0;
                    std::vector<int> sc0, sc1;
                    bool spanOk = false;
                    buildRotTemplate(ld, c.angle, ptm, pmk, pst2, pValid,
                                     &sc0, &sc1, &spanOk);

                    double s[3][3];
                    for (int oy = -1; oy <= 1; oy++)
                    {
                        for (int ox = -1; ox <= 1; ox++)
                        {
                            const cv::Point2f p(cx + ox + ld.centre.x,
                                                cy + oy + ld.centre.y);
                            double v = spanOk
                                ? scoreAtSpan(ld, py[0], fpy[0], ptm, sc0, sc1, pst2, pValid, p)
                                : scoreAtPrepared(ld, py[0], fpy[0], ptm, pmk, pst2, pValid, p);
                            if (params_.metric == 1) v = std::fabs(v);
                            s[oy + 1][ox + 1] = v;
                        }
                    }

                    const double v0 = s[1][1];
                    double dx = 0, dy = 0;
                    double d1 = 2.0 * v0 - s[1][0] - s[1][2];
                    if (d1 > 1e-9)
                        dx = clampD(0.5 * (s[1][2] - s[1][0]) / d1, -1.0, 1.0);
                    double d2 = 2.0 * v0 - s[0][1] - s[2][1];
                    if (d2 > 1e-9)
                        dy = clampD(0.5 * (s[2][1] - s[0][1]) / d2, -1.0, 1.0);
                    c.pos = cv::Point2f((float)(cx + ld.centre.x + dx),
                                        (float)(cy + ld.centre.y + dy));
                }
            }
        });
    }

    //---- report.  Halcon's MaxOverlap decides how many survive ----
    tSub = msSince(t1) - tCoarse - tRefine;
    const cv::Size2f tsz((float)im.gray0.cols, (float)im.gray0.rows);
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.score > b.score; });

    for (size_t i = 0; i < cands.size() && (int)results.size() < fp.maxMatches; i++)
    {
        if (cands[i].score < fp.minScore) break;

        NccResult r;
        r.pos   = cands[i].pos;
        r.angle = cands[i].angle;
        r.score = cands[i].score;

        bool dup = false;
        for (size_t j = 0; j < results.size(); j++)
            if (rectOverlap(tsz, r, results[j]) > fp.maxOverlap) { dup = true; break; }
        if (!dup) results.push_back(r);
    }

    if (fp.debug >= 1)
    {
        size_t cacheBytes = 0;
        for (const auto& kv : im.rotCache)
            cacheBytes += kv.second.tm.total() * kv.second.tm.elemSize()
                        + kv.second.mk.total() * kv.second.mk.elemSize();
        std::printf("[ncc] levels=%d top=%d scanned=%d topAngles=%d cands=%d kept=%d  "
                    "pre=%.2f  search=%.2f ms  [coarse %.2f | refine %.2f | subpix %.2f]\n",
                    maxLev, top, coarseTop, (int)im.levels[coarseTop].angles.size(),
                    (int)cands.size(), (int)results.size(),
                    (double)im.lastPre, (double)im.lastSearch,
                    tCoarse, tRefine, tSub);
        if (fp.debug >= 2)
            std::printf("[ncc] rotCache: %zu entries, %.2f MB (hits %d / miss %d)\n",
                        im.rotCache.size(), cacheBytes / (1024.0 * 1024.0),
                        im.rotCacheHits, im.rotCacheMiss);
    }

    // The reference point (setOrigin) is applied LAST, so the MaxOverlap test
    // above still works on the model centre - the overlap fraction must not
    // depend on where the caller put the reference point.
    if (hasOrigin_ && !results.empty())
    {
        const cv::Point2f ctr = im.levels.empty() ? cv::Point2f(0.f, 0.f)
                                                  : im.levels[0].srcCentre;
        const double dx = (double)origin_.x - ctr.x;
        const double dy = (double)origin_.y - ctr.y;
        if (std::abs(dx) > 1e-6 || std::abs(dy) > 1e-6)
        {
            for (size_t i = 0; i < results.size(); i++)
            {
                NccResult& r = results[i];
                const double ca = std::cos(r.angle), sa = std::sin(r.angle);
                r.pos.x = (float)(r.pos.x + (dx * ca + dy * sa));
                r.pos.y = (float)(r.pos.y + (-dx * sa + dy * ca));
            }
        }
    }

    return results;
}

//=======================================================================
//  model facts
//=======================================================================
cv::Mat NccMatcher::modelGray() const
{
    return created_ ? impl_->gray0 : cv::Mat();
}

cv::Size NccMatcher::modelSize() const
{
    return created_ && !impl_->levels.empty()
               ? cv::Size(impl_->levels[0].gray.cols, impl_->levels[0].gray.rows)
               : cv::Size();
}

cv::Point2f NccMatcher::modelCenter() const
{
    return created_ && !impl_->levels.empty() ? impl_->levels[0].srcCentre
                                              : cv::Point2f();
}

int NccMatcher::numLevels() const
{
    return created_ ? (int)impl_->levels.size() : 0;
}

int NccMatcher::modelPixelCount() const
{
    return created_ && !impl_->levels.empty() ? impl_->levels[0].usedPixels : 0;
}

double NccMatcher::angleStep() const { return created_ ? impl_->angleStep0 : 0.0; }

int NccMatcher::angleCount() const
{
    return created_ && !impl_->levels.empty() ? (int)impl_->levels[0].angles.size() : 0;
}

const NccParams& NccMatcher::params() const { return params_; }

//=======================================================================
//  pose & geometry: same set as geomatch::Matcher (see GeoMatch.h).
//  NCC has no scale dimension, so the mapping is
//      imagePt = NccResult::pos + R(angle) * (tplPt - refPoint())
//  with R(a) = [cos a, sin a; -sin a, cos a] - the convention the search itself
//  uses (buildRotTpl rotates the template by getRotationMatrix2D(srcCentre,
//  +angle), which is exactly this R plus the centre shift).
//=======================================================================
cv::Point2f NccMatcher::refPoint() const
{
    if (hasOrigin_) return origin_;
    return created_ ? modelCenter() : cv::Point2f(0.f, 0.f);
}

void NccMatcher::setOrigin(cv::Point2f tplPoint)
{
    origin_    = tplPoint;
    hasOrigin_ = true;
}

void NccMatcher::resetOrigin()
{
    hasOrigin_ = false;
    origin_    = cv::Point2f(0.f, 0.f);
}

cv::Point2f NccMatcher::origin() const { return refPoint(); }

cv::Matx33d NccMatcher::poseMatrix(const NccResult& r) const
{
    const double ca = std::cos(r.angle), sa = std::sin(r.angle);
    const cv::Point2f o = refPoint();
    // scale is 1 by definition: Halcon's NCC has no scale dimension
    const double tx = (double)r.pos.x - (ca * o.x + sa * o.y);
    const double ty = (double)r.pos.y - (-sa * o.x + ca * o.y);
    return cv::Matx33d( ca,  sa, tx,
                       -sa,  ca, ty,
                       0.0, 0.0, 1.0);
}

cv::Mat NccMatcher::poseAffine(const NccResult& r) const
{
    const cv::Matx33d M = poseMatrix(r);
    return (cv::Mat_<double>(2, 3) << M(0, 0), M(0, 1), M(0, 2),
                                      M(1, 0), M(1, 1), M(1, 2));
}

cv::Point2f NccMatcher::templateToImage(const NccResult& r, cv::Point2f p) const
{
    const cv::Point2f o = refPoint();
    const double ca = std::cos(r.angle), sa = std::sin(r.angle);
    const double u = (double)p.x - o.x, v = (double)p.y - o.y;
    return cv::Point2f((float)(r.pos.x + (ca * u + sa * v)),
                       (float)(r.pos.y + (-sa * u + ca * v)));
}

cv::Point2f NccMatcher::imageToTemplate(const NccResult& r, cv::Point2f p) const
{
    const cv::Point2f o = refPoint();
    const double ca = std::cos(r.angle), sa = std::sin(r.angle);
    const double dx = (double)p.x - r.pos.x, dy = (double)p.y - r.pos.y;
    // the inverse of R is its transpose
    return cv::Point2f((float)((ca * dx - sa * dy) + o.x),
                       (float)((sa * dx + ca * dy) + o.y));
}

std::vector<cv::Point2f> NccMatcher::cornersAt(const NccResult& r) const
{
    std::vector<cv::Point2f> q;
    if (!created_) return q;
    const cv::Size tsz = modelSize();
    const float w = (float)tsz.width, h = (float)tsz.height;
    q.reserve(4);
    q.push_back(templateToImage(r, cv::Point2f(0.f, 0.f)));   // template top-left
    q.push_back(templateToImage(r, cv::Point2f(w,   0.f)));   // top-right
    q.push_back(templateToImage(r, cv::Point2f(w,   h)));     // bottom-right
    q.push_back(templateToImage(r, cv::Point2f(0.f, h)));     // bottom-left
    return q;
}

cv::Rect2f NccMatcher::boundsAt(const NccResult& r) const
{
    const std::vector<cv::Point2f> q = cornersAt(r);
    if (q.empty()) return cv::Rect2f();
    return cv::boundingRect(q);
}

double NccMatcher::lastPreprocessTime() const { return impl_ ? impl_->lastPre : 0.0; }
double NccMatcher::lastSearchTime()     const { return impl_ ? impl_->lastSearch : 0.0; }

//=======================================================================
//  persistence.  Layout (little endian), versioned so that later additions
//  stay readable:
//    header : magic 'NCM1' | u32 version
//    params : i32 numLevels | f64 angleStart | f64 angleExtent | f64 angleStep
//             | i32 metric | u32 hasMask
//    gray0  : i32 cols | i32 rows | bytes
//    levels : u32 nLevels, then per level
//               i32 cols | i32 rows | bytes            (the template pyramid)
//               i32 hasMask | (i32 cols | i32 rows | bytes)
//  Angle grids / canvases are recomputed on load from the params, so they are
//  not stored.
//=======================================================================
namespace
{
    const uint32_t NCM_MAGIC   = 0x314D434E;   // 'NCM1'
    const uint32_t NCM_VERSION = 1;

    struct Writer
    {
        std::vector<uchar> b;
        void raw(const void* p, size_t n)
        {
            const uchar* s = (const uchar*)p;
            b.insert(b.end(), s, s + n);
        }
        void u32(uint32_t v) { raw(&v, 4); }
        void i32(int32_t v)  { raw(&v, 4); }
        void f64(double v)   { raw(&v, 8); }
        void mat(const cv::Mat& m)
        {
            i32(m.cols);
            i32(m.rows);
            if (m.empty()) return;
            cv::Mat c = m.isContinuous() ? m : m.clone();
            raw(c.data, c.total() * c.elemSize());
        }
    };

    struct Reader
    {
        const std::vector<uchar>& b;
        size_t off = 0;
        bool ok = true;
        explicit Reader(const std::vector<uchar>& v) : b(v) {}
        void raw(void* p, size_t n)
        {
            if (!ok || off + n > b.size()) { ok = false; return; }
            std::memcpy(p, b.data() + off, n);
            off += n;
        }
        bool u32(uint32_t& v) { raw(&v, 4); return ok; }
        bool i32(int32_t& v)  { raw(&v, 4); return ok; }
        bool f64(double& v)   { raw(&v, 8); return ok; }
        bool mat(cv::Mat& m, int type)
        {
            int32_t w = 0, h = 0;
            if (!i32(w) || !i32(h)) return false;
            if (w <= 0 || h <= 0 || w > 100000 || h > 100000) return false;
            m.create(h, w, type);
            if (!ok) return false;
            raw(m.data, (size_t)w * h * m.elemSize());
            return ok;
        }
    };
}

//=======================================================================
//  score at one exact pose (no search)
//=======================================================================
double NccMatcher::probe(const cv::Mat& gray, cv::Point2f pos, double angle) const
{
    if (!created_ || gray.empty() || impl_->levels.empty()) return 0.0;
    const cv::Mat g8 = toGray8(gray);
    cv::Mat f32;
    g8.convertTo(f32, CV_32F);
    double v = scoreAtPose(impl_->levels[0], g8, f32, pos, angle);
    if (params_.metric == 1) v = std::fabs(v);
    return v;
}

//=======================================================================
//  serialize / deserialize
//=======================================================================
std::vector<uchar> NccMatcher::serializeNccModel() const
{
    Writer w;
    w.u32(NCM_MAGIC);
    w.u32(NCM_VERSION);

    w.i32(params_.numLevels);
    w.f64(params_.angleStart);
    w.f64(params_.angleExtent);
    w.f64(params_.angleStep);
    w.i32(params_.metric);

    w.mat(impl_->gray0);

    const int nLev = (int)impl_->levels.size();
    w.u32((uint32_t)nLev);
    for (int l = 0; l < nLev; l++)
    {
        w.mat(impl_->levels[l].gray);
        const bool hasMask = !impl_->levels[l].mask.empty();
        w.i32(hasMask ? 1 : 0);
        if (hasMask) w.mat(impl_->levels[l].mask);
    }
    return w.b;
}

bool NccMatcher::deserializeNccModel(const std::vector<uchar>& data)
{
    created_ = false;
    resetOrigin();          // see createNccModel(): one origin per model
    Reader r(data);

    uint32_t magic = 0, ver = 0;
    if (!r.u32(magic) || !r.u32(ver)) return false;
    if (magic != NCM_MAGIC || ver < 1 || ver > NCM_VERSION) return false;

    NccParams p;
    int32_t nl = 0, met = 0;
    if (!r.i32(nl)) return false;
    if (!r.f64(p.angleStart)) return false;
    if (!r.f64(p.angleExtent)) return false;
    if (!r.f64(p.angleStep)) return false;
    if (!r.i32(met)) return false;
    p.numLevels = nl;
    p.metric    = met;

    cv::Mat g0;
    if (!r.mat(g0, CV_8U)) return false;

    uint32_t nLev = 0;
    if (!r.u32(nLev) || nLev == 0 || nLev > 16) return false;

    std::vector<cv::Mat> grays(nLev), masks(nLev);
    for (uint32_t l = 0; l < nLev; l++)
    {
        if (!r.mat(grays[l], CV_8U)) return false;
        int32_t hasMask = 0;
        if (!r.i32(hasMask)) return false;
        if (hasMask != 0)
        {
            if (!r.mat(masks[l], CV_8U)) return false;
        }
    }
    if (!r.ok) return false;

    // rebuild the levels (canvas size and angle grid come from the params, so
    // nothing else has to be stored)
    if (!createNccModel(g0, p)) return false;

    // restore the masks that came out of the file
    if (!masks[0].empty())
    {
        for (size_t l = 0; l < impl_->levels.size() && l < masks.size(); l++)
            impl_->levels[l].mask = masks[l];
        // the mask feeds buildRotTemplate, so rotations built inside the
        // createNccModel() call above are stale now.
        impl_->rotCache.clear();
        impl_->rotCacheHits = impl_->rotCacheMiss = 0;
    }
    params_ = p;
    return true;
}

bool NccMatcher::writeNccModel(const std::string& file) const
{
    if (!created_) return false;
    const std::vector<uchar> b = serializeNccModel();
    std::ofstream f(file.c_str(), std::ios::binary);
    if (!f) return false;
    f.write((const char*)b.data(), (std::streamsize)b.size());
    return f.good();
}

bool NccMatcher::readNccModel(const std::string& file)
{
    std::ifstream f(file.c_str(), std::ios::binary);
    if (!f) return false;
    std::vector<uchar> b((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    if (b.empty()) return false;
    return deserializeNccModel(b);
}

//=======================================================================
//  visualisation
//=======================================================================
cv::Mat NccMatcher::drawModelFeatures(double scaleFactor) const
{
    if (!created_) return cv::Mat();
    const double sf = scaleFactor > 0 ? scaleFactor : 1.0;

    cv::Mat color;
    cv::cvtColor(impl_->gray0, color, cv::COLOR_GRAY2BGR);
    if (sf != 1.0)
        cv::resize(color, color, cv::Size(), sf, sf, cv::INTER_NEAREST);

    // mask: pixels that do NOT participate are tinted, so the used region is
    // obvious at a glance
    if (!impl_->levels.empty() && !impl_->levels[0].mask.empty())
    {
        cv::Mat mk;
        cv::resize(impl_->levels[0].mask, mk, color.size(), 0, 0, cv::INTER_NEAREST);
        cv::Mat tint;
        cv::addWeighted(color, 0.30, cv::Mat(color.size(), color.type(), cv::Scalar(120, 60, 0)),
                        0.70, 0, tint);
        tint.copyTo(color, (mk == 0));
    }

    // centre cross
    const cv::Point2f c = impl_->levels[0].srcCentre;
    const cv::Point pc(cvRound(c.x * sf), cvRound(c.y * sf));
    const int arm = (int)std::max(4.0, 6.0 * sf);
    cv::line(color, cv::Point(pc.x - arm, pc.y), cv::Point(pc.x + arm, pc.y),
             cv::Scalar(255, 0, 0), 1, cv::LINE_AA);
    cv::line(color, cv::Point(pc.x, pc.y - arm), cv::Point(pc.x, pc.y + arm),
             cv::Scalar(255, 0, 0), 1, cv::LINE_AA);
    return color;
}

//=======================================================================
//  search image with the matched rotated rectangle + score caption
//=======================================================================
cv::Mat NccMatcher::drawResult(const cv::Mat& gray, const std::vector<NccResult>& res) const
{
    if (gray.empty()) return cv::Mat();

    cv::Mat src = gray;
    if (src.channels() == 3) cv::cvtColor(src, src, cv::COLOR_BGR2GRAY);
    cv::Mat vis;
    cv::cvtColor(src, vis, cv::COLOR_GRAY2BGR);

    if (!created_) return vis;

    const cv::Size tsz = modelSize();
    for (size_t k = 0; k < res.size(); k++)
    {
        const NccResult& r = res[k];
        const cv::Scalar col = (k == 0) ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 200, 255);

        // The box is drawn around the MODEL CENTRE.  r.pos is the reference
        // point (template centre unless setOrigin() moved it), so map it back.
        const cv::Point2f ctr = templateToImage(r, modelCenter());

        // Halcon draws the rotated model rectangle; same pose mapping as the
        // matcher, i.e. a rotation of -angle in image coordinates
        cv::RotatedRect rr(ctr, cv::Size2f((float)tsz.width, (float)tsz.height),
                           -r.angle * 180.0 / kPi);
        cv::Point2f q[4];
        rr.points(q);
        std::vector<cv::Point> poly;
        poly.reserve(4);
        for (int i = 0; i < 4; i++) poly.push_back(cv::Point(cvRound(q[i].x), cvRound(q[i].y)));
        cv::polylines(vis, poly, true, col, 2, cv::LINE_AA);

        cv::circle(vis, cv::Point(cvRound(r.pos.x), cvRound(r.pos.y)), 2, col, -1, cv::LINE_AA);
        cv::line(vis, cv::Point(cvRound(r.pos.x) - 6, cvRound(r.pos.y)),
                 cv::Point(cvRound(r.pos.x) + 6, cvRound(r.pos.y)), col, 1, cv::LINE_AA);

        char cap[160];
        std::snprintf(cap, sizeof(cap), "#%d  ncc=%.3f  a=%.1fdeg  (%.2f;%.2f)",
                      (int)k, r.score, r.angle * 180.0 / kPi, r.pos.x, r.pos.y);
        const cv::Rect box = cv::boundingRect(poly);
        const cv::Size ts = cv::getTextSize(cap, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, nullptr);
        int tx = std::min(std::max(box.x, 2), std::max(2, vis.cols - ts.width - 4));
        int ty = std::min(std::max(box.y + box.height + ts.height + 6, ts.height + 4),
                          vis.rows - 4);
        cv::rectangle(vis, cv::Rect(tx - 2, ty - ts.height - 2, ts.width + 4, ts.height + 4),
                      cv::Scalar(0, 0, 0), -1);
        cv::putText(vis, cap, cv::Point(tx, ty), cv::FONT_HERSHEY_SIMPLEX, 0.5, col, 1, cv::LINE_AA);
    }

    char hdr[160];
    std::snprintf(hdr, sizeof(hdr), "NCC matches=%d   |   rotated box = template, green=#0",
                  (int)res.size());
    cv::rectangle(vis, cv::Rect(0, 0, vis.cols, 24), cv::Scalar(0, 0, 0), -1);
    cv::putText(vis, hdr, cv::Point(6, 17), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
    return vis;
}

} // namespace geomatch
