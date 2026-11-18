//***********************************************************************
//  Halcon-like shape based template matching (edge/gradient based)
//***********************************************************************
#include "ShapeMatch/ShapeMatch.h"

#include <opencv2/core/ocl.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <iostream>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#  define SM_USE_SSE2 1
#  include <emmintrin.h>
#else
#  define SM_USE_SSE2 0
#endif

// 256-bit path.  MSVC does not predefine __AVX2__, it only honours /arch:AVX2
// at code generation time, so the macro is defined by the build system below.
#if !defined(SM_USE_AVX2)
#  if defined(__AVX2__)
#    define SM_USE_AVX2 1   // /arch:AVX2 was given (MSVC defines this for x86
                             // and x64 alike -- verified on the 32-bit build,
                             // which printed "__AVX2__ defined")
#  elif defined(_M_X64)
#    define SM_USE_AVX2 1   // x64 baseline
#  else
#    define SM_USE_AVX2 0
#  endif
#endif
#if SM_USE_AVX2
#  include <immintrin.h>
#endif

// reports what this translation unit was really compiled with (see the header)
int shapeSimdWidth()
{
#if SM_USE_AVX2
    return 256;
#else
    return 0;
#endif
}

// restrict qualifier: MSVC spells it __restrict, gcc/clang __restrict__
#if !defined(SM_RESTRICT)
#  if defined(_MSC_VER)
#    define SM_RESTRICT __restrict
#  else
#    define SM_RESTRICT __restrict__
#  endif
#endif

//-----------------------------------------------------------------------
//  两个角度的最短角差（归一化到 [0, PI]），处理 ±180° 环绕。
//  +180° 和 -180° 其实是同一个位姿，直接相减会得到约 2*PI（约 6.28 rad），
//  去重时会误判成两个不同的结果；这里折叠到 [0, PI] 后约等于 0。
//-----------------------------------------------------------------------
static inline double angleDiffWrapped(double a, double b)
{
    double d = std::abs(a - b);
    while (d > CV_PI) d -= 2.0 * CV_PI;
    return d;
}

//-----------------------------------------------------------------------
//  IEEE-754 half (FP16) helpers.
//  The normalised gradients of the search image are stored as halves so the
//  scan loop, which is memory bound, moves only half the bytes.
//-----------------------------------------------------------------------
static inline uint16_t f32ToF16(float v)
{
#if SM_USE_AVX2
    return (uint16_t)_mm_cvtsi128_si32(
        _mm_cvtps_ph(_mm_set_ss(v), _MM_FROUND_TO_NEAREST_INT));
#else
    uint32_t bits; std::memcpy(&bits, &v, 4);
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x7FFFFFu;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);          // inf / nan
    if (exp <= 0)                                               // too small -> zero
        return (uint16_t)sign;
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
#endif
}

static inline float f16ToF32(uint16_t h)
{
#if SM_USE_AVX2
    return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128((int)h)));
#else
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0)
    {
        if (mant == 0) bits = sign;                             // +-0
        else
        {                                                       // subnormal
            int32_t e = -14;
            while ((mant & 0x400u) == 0) { mant <<= 1; e++; }
            mant &= 0x3FFu;
            bits = sign | ((uint32_t)(e + 127) << 23) | (mant << 13);
        }
    }
    else if (exp == 31)
        bits = sign | 0x7F800000u | (mant << 13);               // inf / nan
    else
        bits = sign | ((uint32_t)((int)exp + 112) << 23) | (mant << 13);
    float out; std::memcpy(&out, &bits, 4);
    return out;
#endif
}

#ifndef CV_PI
#define CV_PI 3.1415926535897932384626433832795
#endif

//-----------------------------------------------------------------------
//  8-bit quantised coarse scan (see ScanInvoker8).
//
//  image gradient:  q  = round(g * 127)      |g| <= 1  ->  q in [-127, 127]
//  model normal  :  nq = round(n * 258)      |n| = 1   ->  nq in [-258, 258]
//
//  one vpmaddwd of the interleaved (qx,qy) pairs with the broadcast (nqx,nqy)
//  pattern gives int32 dots that are exactly
//
//      qx*nqx + qy*nqy = 127*258 * (gx*nx + gy*ny)
//
//  so the float score is sum/(127*258*cnt).  |one point| <= 32766, which keeps
//  the int32 accumulator safe for up to ~65000 model points.
//-----------------------------------------------------------------------
enum { kQ8 = 127, kN16 = 258 };
static const float kQ8Dot = (float)(kQ8 * kN16);      // 32766
// guard: int32 accumulator (n*32766) and the int16 kept-count (n) both fit
static const int   kQ8MaxPoints = 30000;

struct XForm
{
    std::vector<int16_t> ox, oy;   // cvRound of the rotated offset (coarse scan)
    std::vector<float>   fx, fy;   // exact rotated offset (sub-pixel sampling)
    std::vector<float>   nx, ny;
    // Normal of every point quantised for the integer (vpmaddwd) scan:
    //   np[k] = (int16)(ny*258) << 16 | (uint16)(int16)(nx*258)
    // so a broadcast of np[k] is exactly the [nx,ny,nx,ny,...] int16 pattern
    // the interleaved int8 gradient pairs need.  258 keeps |n| <= 258, hence
    // one point contributes at most 127*258 = 32766 to the int32 accumulator.
    std::vector<int32_t> np;
    int  R = 0;
    int  n = 0;
    cv::Mat kx, ky;
    bool hasKernel = false;
};

// (re)build the integer normal packets of an XForm from its float normals.
// Called by buildXForm and whenever the normals are negated (global polarity
// scan), so the 8-bit kernel can never use a stale packet.
inline void quantizeNormals(XForm& xf)
{
    const int n = (int)xf.nx.size();
    xf.np.resize(n);
    for (int k = 0; k < n; k++)
    {
        const int32_t nqx = (int32_t)cvRound(xf.nx[k] * (float)kN16);
        const int32_t nqy = (int32_t)cvRound(xf.ny[k] * (float)kN16);
        xf.np[k] = (int32_t)((int16_t)nqy << 16) | (int32_t)(uint16_t)(int16_t)nqx;
    }
}

namespace
{
    inline uint64_t makeKey(int level, int aIdx, int sIdx)
    {
        return (uint64_t)(level) << 44 | (uint64_t)(aIdx & 0xFFFFF) << 20 | (uint64_t)(sIdx & 0xFFFFF);
    }

    // milliseconds elapsed since t0, for the phase report (debug >= 2)
    inline double msSince(int64 t0)
    {
        return (double)(cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();
    }

    // Coarse-scan scheduling: how many parallel stripes the (angle x scale)
    // pose list is split into.  One stripe per pose lets the thread pool spread
    // the poses over all cores; the old "poses/8" collapsed a short pose list
    // into a single stripe and ran the whole coarse scan serial.
    // SM_COARSE_STRIPES=0 restores that old grouping for A/B comparison.
    double coarseStripes(size_t nPoses)
    {
        static const bool grouped = (std::getenv("SM_COARSE_STRIPES") != nullptr &&
                                     std::atoi(std::getenv("SM_COARSE_STRIPES")) == 0);
        const double n = std::max(1.0, (double)nPoses);
        return grouped ? std::max(1.0, n / 8.0) : n;
    }

    bool oclUsable()
    {
        static int cached = -1;
        if (cached < 0)
        {
            cached = 0;
            if (cv::ocl::haveOpenCL() && cv::ocl::useOpenCL())
            {
                cv::ocl::Context ctx;
                cached = ctx.ndevices() > 0 ? 1 : 0;
            }
        }
        return cached == 1;
    }
}

struct ShapeMatcher::Impl
{
    std::unordered_map<uint64_t, XForm> xformCache;
    int cacheLimit = 2048;
};

ShapeMatcher::ShapeMatcher()
    : created_(false), useOCL_(false),
      lastPreprocessTime_(0), lastSearchTime_(0)
{
    impl_ = new Impl();
}

ShapeMatcher::~ShapeMatcher()
{
    delete impl_;
}

bool ShapeMatcher::openCLAvailable()
{
    return oclUsable();
}

//=======================================================================
//  model creation
//=======================================================================
bool ShapeMatcher::createShapeModel(const cv::Mat& gray, const MatchParams& p)
{
    params_ = p;
    created_ = false;
    modelLevels_.clear();
    impl_->xformCache.clear();

    CV_Assert(!gray.empty() && gray.channels() == 1);
    gray.convertTo(modelGray0_, CV_8U);

    // ---- normalise the mask to the level-0 template ---------------------
    cv::Mat mask0;
    if (!params_.mask.empty())
    {
        cv::Mat m1;
        if (params_.mask.channels() == 3)
            cv::cvtColor(params_.mask, m1, cv::COLOR_BGR2GRAY);
        else
            params_.mask.convertTo(m1, CV_8U);
        if (m1.size() != modelGray0_.size())
            cv::resize(m1, mask0, modelGray0_.size(), 0, 0, cv::INTER_NEAREST);
        else
            m1.copyTo(mask0);
        cv::threshold(mask0, mask0, 0, 255, cv::THRESH_BINARY);
    }

    // ---- pyramid levels -------------------------------------------------
    // 最粗一层的最短边下限，可配置（MatchParams::minSide）。钳到 [4, 256]：
    // 小于 4 时 pyrDown 会把模板糊成几个像素、边缘点全糊掉；大于 256 等于
    // 禁用金字塔（只剩 level 0），只会更慢。
    const int minSide = std::max(4, std::min(256, params_.minSide));
    int lv = 0;
    std::vector<cv::Mat> pyr;
    pyr.push_back(modelGray0_);
    while (true)
    {
        if (params_.numLevels > 0 && (int)pyr.size() >= params_.numLevels)
            break;
        cv::Mat next;
        cv::pyrDown(pyr.back(), next);
        if (next.cols < minSide || next.rows < minSide)
            break;
        pyr.push_back(next);
        if (++lv > 6) break;
    }
    int nLevels = (int)pyr.size();

    // ---- edge points per level -----------------------------------------
    modelLevels_.resize(nLevels);
    for (int i = 0; i < nLevels; i++)
    {
        modelLevels_[i].size = cv::Size(pyr[i].cols, pyr[i].rows);
        cv::Mat mlev;
        if (!mask0.empty())
        {
            if (i == 0) mlev = mask0;
            else cv::resize(mask0, mlev, pyr[i].size(), 0, 0, cv::INTER_NEAREST);
        }
        selectEdgePoints(modelLevels_[i], pyr[i], mlev, i);
    }
    if (modelLevels_[0].points.empty())
        return false;

    buildAngleScaleGrids();
    created_ = true;
    return true;
}

namespace
{
    // keep a pixel only when it is the maximum along its own gradient
    // direction (quantised to 0/45/90/135 deg) -> one pixel wide ridges
    void thinRidges(const cv::Mat& mag, const cv::Mat& gx, const cv::Mat& gy, cv::Mat& thin)
    {
        const int w = mag.cols, h = mag.rows;
        thin.create(h, w, CV_8U);
        thin.setTo(0);

        for (int y = 1; y < h - 1; y++)
        {
            const float* pm = mag.ptr<float>(y);
            const float* px = gx.ptr<float>(y);
            const float* py = gy.ptr<float>(y);
            uchar* pt = thin.ptr<uchar>(y);
            for (int x = 1; x < w - 1; x++)
            {
                const float m = pm[x];
                if (m <= 0.f) continue;
                const float dx = px[x], dy = py[x];
                const float adx = std::fabs(dx), ady = std::fabs(dy);
                const int sx = dx >= 0.f ? 1 : -1, sy = dy >= 0.f ? 1 : -1;
                int ox = 0, oy = 0;
                if (adx >= ady) { ox = sx; if (ady > 0.41421356f * adx) oy = sy; }
                else            { oy = sy; if (adx > 0.41421356f * ady) ox = sx; }
                if (m >= mag.ptr<float>(y + oy)[x + ox] &&
                    m >= mag.ptr<float>(y - oy)[x - ox])
                    pt[x] = 1;
            }
        }
    }

    // hysteresis: a ridge that reaches hi is a seed, everything connected to
    // it stays while it is above lo -> weak but continuous edges survive
    // Halcon Contrast 的「多段幅值区间」：非空时种子和连接都要求幅值落在
    // 任意一段 [lo,hi] 里（含上下界）。空 = 老的单段滞后阈值行为。
    typedef std::vector<std::pair<float, float>> Bands;
    inline bool inBands(const Bands& b, float v)
    {
        for (size_t i = 0; i < b.size(); i++)
            if (v >= b[i].first && v <= b[i].second) return true;
        return false;
    }

    void growEdges(const cv::Mat& mag, const cv::Mat& thin, float lo, float hi, cv::Mat& edge,
                   const Bands* bands = nullptr)
    {
        const int w = mag.cols, h = mag.rows;
        edge.create(h, w, CV_8U);
        edge.setTo(0);
        const float* pm = mag.ptr<float>();
        const uchar* pt = thin.ptr<uchar>();

        uchar* pe = edge.ptr<uchar>();
        std::vector<int> stack;
        const bool useBands = (bands && !bands->empty());
        // 单段口径没有幅值上限（历史行为）；多段口径要求落在某一段内。
        for (int i = 0; i < w * h; i++)
        {
            if (!pt[i] || pe[i]) continue;
            const bool seed = useBands ? inBands(*bands, pm[i]) : (pm[i] >= hi);
            if (!seed) continue;
            pe[i] = 1;
            stack.push_back(i);
        }

        while (!stack.empty())
        {
            const int i = stack.back();
            stack.pop_back();
            const int y = i / w, x = i - y * w;
            for (int dy = -1; dy <= 1; dy++)
            {
                const int yy = y + dy;
                if (yy < 0 || yy >= h) continue;
                uchar* er = edge.ptr<uchar>(yy);
                const uchar* tr = thin.ptr<uchar>(yy);
                const float* mr = mag.ptr<float>(yy);
                for (int dx = -1; dx <= 1; dx++)
                {
                    const int xx = x + dx;
                    if (xx < 0 || xx >= w) continue;
                    if (er[xx] || !tr[xx]) continue;
                    if (useBands ? !inBands(*bands, mr[xx]) : (mr[xx] < lo)) continue;
                    er[xx] = 1;
                    stack.push_back(yy * w + xx);
                }
            }
        }
    }

    // walk the thinned edges into chains and take the points at equal arc
    // length, so the model follows the texture instead of clustering on the
    // few strongest ridges (this is what Halcon's model contours look like).
    // maxPts <= 0 = no budget: every chain pixel is kept (see noCap below).
    // fullChains, when not null, receives every kept chain's pixels in
    // arc-length order - that is the contour the model is drawn with.
    std::vector<int> sampleAlongContours(const cv::Mat& edge, const cv::Mat& mag, int maxPts,
                                         int minChainLen,
                                         std::vector<std::vector<int>>* fullChains = nullptr)
    {
        if (fullChains) fullChains->clear();
        const int w = edge.cols, h = edge.rows, nPix = w * h;
        const uchar* pe = edge.ptr<uchar>();
        const float* pm = mag.ptr<float>();
        std::vector<uchar> used((size_t)nPix, 0);
        std::vector<std::vector<int>> chains;

        auto degree = [&](int i)
        {
            const int y = i / w, x = i - y * w;
            int d = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    if (!dy && !dx) continue;
                    const int yy = y + dy, xx = x + dx;
                    if (yy < 0 || yy >= h || xx < 0 || xx >= w) continue;
                    if (pe[yy * w + xx]) d++;
                }
            return d;
        };

        auto trace = [&](int start)
        {
            std::vector<int> ch;
            for (int cur = start; cur >= 0;)
            {
                used[cur] = 1;
                ch.push_back(cur);
                const int y = cur / w, x = cur - y * w;
                int nxt = -1;
                float best = -1.f;
                for (int dy = -1; dy <= 1; dy++)
                {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        if (!dy && !dx) continue;
                        const int xx = x + dx;
                        if (xx < 0 || xx >= w) continue;
                        const int j = yy * w + xx;
                        if (!pe[j] || used[j]) continue;
                        if (pm[j] > best) { best = pm[j]; nxt = j; }
                    }
                }
                cur = nxt;
            }
            if ((int)ch.size() >= minChainLen) chains.push_back(std::move(ch));
        };

        for (int i = 0; i < nPix; i++)
            if (pe[i] && !used[i] && degree(i) == 1) trace(i);
        for (int i = 0; i < nPix; i++)
            if (pe[i] && !used[i]) trace(i);

        std::vector<int> out;
        if (chains.empty()) return out;

        // maxPts <= 0 means "no budget": every pixel of the hysteresis result
        // becomes a model point.  This is what create_shape_model does (it has
        // no point cap), and it keeps the strong-edge coverage of a large or
        // dense template intact instead of thinning it down to a skeleton.
        const bool noCap = (maxPts <= 0);

        // the budget goes to a chain by its contrast (length x strength), not
        // by its length alone, otherwise a long faint chain eats the points
        std::vector<double> weight(chains.size(), 0.0);
        double total = 0.0;
        for (size_t k = 0; k < chains.size(); k++)
        {
            double w = 0.0;
            for (int i : chains[k]) w += pm[i];
            weight[k] = w;
            total += w;
        }
        if (total <= 0.0) total = 1.0;

        if (!noCap && (int)chains.size() > maxPts)
        {
            std::vector<int> order(chains.size());
            for (size_t k = 0; k < order.size(); k++) order[k] = (int)k;
            std::sort(order.begin(), order.end(),
                      [&](int a, int b) { return weight[a] > weight[b]; });
            order.resize(maxPts);
            std::sort(order.begin(), order.end());
            std::vector<std::vector<int>> keep;
            std::vector<double> kw;
            for (int k : order) { keep.push_back(std::move(chains[k])); kw.push_back(weight[k]); }
            chains.swap(keep);
            weight.swap(kw);
        }

        std::vector<int> quota(chains.size());
        int assigned = 0;
        if (noCap)
        {
            // keep the whole chain, arc length included
            for (size_t k = 0; k < chains.size(); k++)
            {
                quota[k] = (int)chains[k].size();
                assigned += quota[k];
            }
        }
        else
        {
            for (size_t k = 0; k < chains.size(); k++)
            {
                const int len = (int)chains[k].size();
                quota[k] = std::min(len, std::max(1, (int)std::llround((double)maxPts * weight[k] / total)));
                assigned += quota[k];
            }
            while (assigned > maxPts)
            {
                size_t big = 0;
                for (size_t k = 1; k < chains.size(); k++)
                    if (quota[k] > quota[big]) big = k;
                if (quota[big] <= 1) break;
                quota[big]--;
                assigned--;
            }
            // a chain cannot spend more points than it has pixels, so the rest
            // moves on to the chains that still have room
            for (int pass = 0; pass < 4 && assigned < maxPts; pass++)
            {
                double freeW = 0.0;
                for (size_t k = 0; k < chains.size(); k++)
                    if (quota[k] < (int)chains[k].size()) freeW += weight[k];
                if (freeW <= 0.0) break;
                for (size_t k = 0; k < chains.size() && assigned < maxPts; k++)
                {
                    const int room = (int)chains[k].size() - quota[k];
                    if (room <= 0) continue;
                    const int rest = maxPts - assigned;
                    const int add = std::min(room, (int)std::llround((double)rest * weight[k] / freeW));
                    quota[k] += add;
                    assigned += add;
                }
            }
        }

        out.reserve((size_t)std::max(assigned, 0));

        for (size_t k = 0; k < chains.size(); k++)
        {
            const std::vector<int>& ch = chains[k];
            for (int j = 0; j < quota[k]; j++)
            {
                const size_t t = (size_t)((double)(j + 0.5) * ch.size() / quota[k]);
                out.push_back(ch[std::min(t, ch.size() - 1)]);
            }
        }

        // hand the full chains over before the point list is reordered; the
        // contours are pure display data and independent of that order
        if (fullChains)
        {
            fullChains->reserve(chains.size());
            for (size_t k = 0; k < chains.size(); k++)
                if ((int)chains[k].size() >= minChainLen && (int)chains[k].size() >= 2)
                    fullChains->push_back(chains[k]);
        }

        // strongest first: the scan loop prunes on the first points it sums up
        std::stable_sort(out.begin(), out.end(), [pm](int a, int b) { return pm[a] > pm[b]; });
        return out;
    }
}

void ShapeMatcher::selectEdgePoints(LevelData& ld, const cv::Mat& grayLevel, const cv::Mat& mask, int level) const
{
    cv::Mat gx, gy, mag, thin, edge;
    cv::Sobel(grayLevel, gx, CV_32F, 1, 0, 3);
    cv::Sobel(grayLevel, gy, CV_32F, 0, 1, 3);
    cv::magnitude(gx, gy, mag);

    const int w = grayLevel.cols, h = grayLevel.rows;
    const float* pMag = mag.ptr<float>();
    const float* pGx = gx.ptr<float>();
    const float* pGy = gy.ptr<float>();

    // the number of model points is kept roughly constant over the levels.
    // maxPoints <= 0 means "no budget": the finest level keeps every hysteresis
    // pixel (Halcon's create_shape_model has no point cap either) and the
    // coarser levels scale down from that natural count by 2^level, so their
    // share of the points stays the same as with an explicit budget.
    int maxPts = params_.maxPoints;
    // Halcon 的 Optimization 映射到点数预算（见 MatchParams::optimization）：
    // 目的只是让配方能原样搬过来，本库真正的控点旋钮仍是 maxPoints。
    if (maxPts > 0 && !params_.optimization.empty())
    {
        const std::string& o = params_.optimization;
        double f = 1.0;
        if      (o == "point_reduction_low")    f = 0.75;
        else if (o == "point_reduction_medium") f = 0.50;
        else if (o == "point_reduction_high")   f = 0.33;
        // "none" / "auto" / 其它 -> f = 1（不改）
        maxPts = std::max(32, (int)std::lround(maxPts * f));
    }
    if (maxPts <= 0)
        maxPts = (level == 0) ? 0 : std::max(64, (int)modelLevels_[0].points.size() / (1 << level));
    else if (level > 0)
        maxPts = std::max(64, maxPts / (1 << level));

    // origin = centre of the model image, mapped to this pyramid level and
    // snapped to a full pixel (otherwise the rounding of the offsets and the
    // rounding of the match position would add up to a one pixel error)
    const int icx = (modelGray0_.cols / 2) >> level;
    const int icy = (modelGray0_.rows / 2) >> level;
    ld.center = cv::Point2f((float)icx, (float)icy);
    ld.points.clear();
    ld.activePoints = 0;
    if (level == 0) { candCount_ = 0; strongestMag_ = 0; weakestMag_ = 0; }

    // Hysteresis only needs hi >= lo; the old "lo * 1.5" floor silently raised
    // the caller's high threshold.  That made the high threshold impossible to
    // lower below 1.5 x lo, and made a large lo find no seed at all (this
    // template's strongest Sobel response is 152, so hi >= 180 -> 0 points).
    // Keep the caller's value, but not below lo, otherwise growEdges has no
    // seed.  hi == lo degenerates to a plain threshold, which is exactly what
    // Halcon does when Contrast is given as a single value.
    const float lo = (float)std::max(params_.minContrast, 1.0);
    const float hi = (float)std::max(params_.contrast, (double)lo);
    thinRidges(mag, gx, gy, thin);
    // 多段幅值区间（Halcon Contrast 的元组语义）：模板里存的 Contrast 是
    // Sobel 幅值，界面上的 "灰度差" 已经乘过 kSobelPerGray，所以这里直接用。
    Bands bands;
    if (params_.contrastBands.size() >= 2)
    {
        bands.reserve(params_.contrastBands.size() / 2);
        for (size_t i = 0; i + 1 < params_.contrastBands.size(); i += 2)
        {
            float a = (float)params_.contrastBands[i];
            float b = (float)params_.contrastBands[i + 1];
            if (a > b) std::swap(a, b);
            bands.push_back(std::make_pair(std::max(0.f, a), b));
        }
    }
    growEdges(mag, thin, lo, hi, edge, bands.empty() ? nullptr : &bands);

    // mask off edges outside the region of interest
    if (!mask.empty())
        cv::bitwise_and(edge, mask, edge);

    std::vector<std::vector<int>> fullChains;
    const std::vector<int> idx = sampleAlongContours(edge, mag, maxPts, params_.minChainLen,
                                                    &fullChains);
    const int n = (int)idx.size();

    ld.points.resize(n);
    for (int k = 0; k < n; k++)
    {
        const int i = idx[k];
        const int yy = i / w, xx = i - yy * w;
        ModelPoint& mp = ld.points[k];
        mp.x = (float)xx - icx;
        mp.y = (float)yy - icy;
        mp.gx = pGx[i];
        mp.gy = pGy[i];
        mp.mag = pMag[i];
    }
    ld.activePoints = n;

    // contours for display: full chains converted to centre-relative pixels
    ld.chains.clear();
    ld.chains.reserve(fullChains.size());
    for (size_t c = 0; c < fullChains.size(); c++)
    {
        std::vector<cv::Point> poly;
        poly.reserve(fullChains[c].size());
        for (size_t k = 0; k < fullChains[c].size(); k++)
        {
            const int i = fullChains[c][k];
            const int yy = i / w, xx = i - yy * w;
            poly.push_back(cv::Point(xx - icx, yy - icy));
        }
        ld.chains.push_back(std::move(poly));
    }

    if (level == 0 && n > 0)
    {
        candCount_    = cv::countNonZero(edge);
        strongestMag_ = pMag[idx.front()];
        weakestMag_   = pMag[idx.back()];
        computeGeometryFacts(ld);
    }
}

//=======================================================================
//  angle / scale grids
//=======================================================================
void ShapeMatcher::buildAngleScaleGrids()
{
    const LevelData& l0 = modelLevels_[0];
    float rmax = 1.f;
    for (size_t k = 0; k < l0.points.size(); k++)
    {
        const ModelPoint& mp = l0.points[k];
        float r = std::sqrt(mp.x * mp.x + mp.y * mp.y);
        if (r > rmax) rmax = r;
    }

    // finest step: the outermost model point moves at most ~1 pixel
    double aStep = 1.0 / rmax;
    double sStep = 1.0 / rmax;
    if (params_.angleStep > 0) aStep = params_.angleStep;
    if (params_.scaleStep > 0) sStep = params_.scaleStep;
    aStep = std::min(std::max(aStep, 0.0087), 0.0873);  // 0.5deg .. 5deg
    sStep = std::min(std::max(sStep, 0.005), 0.10);

    int nLevels = (int)modelLevels_.size();
    angleStep_ = aStep;
    scaleStep_ = sStep;

    int nAng = (int)std::floor(params_.angleExtent / aStep) + 1;
    if (nAng < 1) nAng = 1;
    angleGrid_.resize(nAng);
    for (int i = 0; i < nAng; i++)
        angleGrid_[i] = params_.angleStart + i * aStep;

    double sMin = std::min(params_.scaleMin, params_.scaleMax);
    double sMax = std::max(params_.scaleMin, params_.scaleMax);
    int nSca = (int)std::floor((sMax - sMin) / sStep) + 1;
    if (nSca < 1) nSca = 1;
    scaleGrid_.resize(nSca);
    for (int i = 0; i < nSca; i++)
        scaleGrid_[i] = sMin + i * sStep;
    scaleMin_ = sMin;

    // per level the usable step is 2^level times coarser
    levelAngleStep_.resize(nLevels);
    levelScaleStep_.resize(nLevels);
    levelAngleCount_.resize(nLevels);
    levelScaleCount_.resize(nLevels);
    for (int l = 0; l < nLevels; l++)
    {
        int stride = 1 << l;
        levelAngleStep_[l] = aStep * stride;
        levelScaleStep_[l] = sStep * stride;
        levelAngleCount_[l] = (nAng + stride - 1) / stride;
        levelScaleCount_[l] = (nSca + stride - 1) / stride;
    }
}

//=======================================================================
//  rotated / scaled model (feature list + dense correlation kernel)
//=======================================================================
void ShapeMatcher::buildXForm(int level, int aIdx, int sIdx, XForm& xf) const
{
    const LevelData& ld = modelLevels_[level];
    const int n = ld.activePoints;
    const double a = angleGrid_[aIdx];
    const double s = scaleGrid_[sIdx];
    const float ca = (float)std::cos(a), sa = (float)std::sin(a);

    xf.ox.resize(n); xf.oy.resize(n);
    xf.fx.resize(n); xf.fy.resize(n);
    xf.nx.resize(n); xf.ny.resize(n);
    xf.np.resize(n);
    int R = 0;
    for (int k = 0; k < n; k++)
    {
        const ModelPoint& mp = ld.points[k];
        // rotation matrix [[ca, sa], [-sa, ca]] == cv::getRotationMatrix2D convention
        float cx = (float)(s * (mp.x * ca + mp.y * sa));
        float cy = (float)(s * (-mp.x * sa + mp.y * ca));
        float inv = (mp.mag > 1e-6f) ? (1.f / mp.mag) : 0.f;
        float vx = (mp.gx * ca + mp.gy * sa) * inv;
        float vy = (-mp.gx * sa + mp.gy * ca) * inv;
        int ix = cvRound(cx), iy = cvRound(cy);
        xf.ox[k] = (int16_t)ix;
        xf.oy[k] = (int16_t)iy;
        // exact (sub-pixel) rotated offset, used by the built-in bilinear scoring
        xf.fx[k] = cx;
        xf.fy[k] = cy;
        xf.nx[k] = vx;
        xf.ny[k] = vy;
        int m = std::max(std::abs(ix), std::abs(iy));
        if (m > R) R = m;
    }
    xf.R = R;
    xf.n = n;
    // integer counterpart of the normals for the 8-bit scan
    quantizeNormals(xf);
    xf.hasKernel = false;
}

void ShapeMatcher::buildKernel(XForm& xf) const
{
    const int R = xf.R;
    const int K = 2 * R + 1;
    cv::Mat ax = cv::Mat::zeros(K, K, CV_32F);
    cv::Mat ay = cv::Mat::zeros(K, K, CV_32F);
    cv::Mat cnt = cv::Mat::zeros(K, K, CV_32S);

    for (int k = 0; k < xf.n; k++)
    {
        int i = xf.oy[k] + R, j = xf.ox[k] + R;
        ax.at<float>(i, j) += xf.nx[k];
        ay.at<float>(i, j) += xf.ny[k];
        cnt.at<int>(i, j) += 1;
    }
    for (int i = 0; i < K; i++)
    {
        const int* pc = cnt.ptr<int>(i);
        float* px = ax.ptr<float>(i);
        float* py = ay.ptr<float>(i);
        for (int j = 0; j < K; j++)
        {
            if (pc[j] > 1)
            {
                float f = 1.f / pc[j];
                px[j] *= f;
                py[j] *= f;
            }
        }
    }
    cv::flip(ax, xf.kx, -1);
    cv::flip(ay, xf.ky, -1);
    xf.hasKernel = true;
}

XForm& ShapeMatcher::getXFormImpl(int level, int aIdx, int sIdx, bool allowEvict)
{
    uint64_t key = makeKey(level, aIdx, sIdx);
    Impl& im = *impl_;
    auto it = im.xformCache.find(key);
    if (it != im.xformCache.end())
        return it->second;
    // evicting here would drop entries a parallel scan is about to read, so
    // the warm up pass must not trigger it
    if (allowEvict && (int)im.xformCache.size() > im.cacheLimit)
        im.xformCache.clear();

    XForm xf;
    buildXForm(level, aIdx, sIdx, xf);
    auto res = im.xformCache.insert(std::make_pair(key, std::move(xf)));
    return res.first->second;
}

XForm& ShapeMatcher::getXForm(int level, int aIdx, int sIdx)
{
    return getXFormImpl(level, aIdx, sIdx, true);
}

XForm& ShapeMatcher::warmXForm(int level, int aIdx, int sIdx)
{
    return getXFormImpl(level, aIdx, sIdx, false);
}

// no insertion, no eviction -> safe to call from every worker thread
const XForm* ShapeMatcher::findXForm(int level, int aIdx, int sIdx) const
{
    const uint64_t key = makeKey(level, aIdx, sIdx);
    const Impl& im = *impl_;
    auto it = im.xformCache.find(key);
    return it == im.xformCache.end() ? nullptr : &it->second;
}

//=======================================================================
//  search image preparation (pyramid + normalized gradients)
//=======================================================================
namespace
{
    template <class T>
    void normGradients(const T& gray, T& sx, T& sy, double minContrast)
    {
        T gx, gy, mag, den, tx, ty;
        cv::Sobel(gray, gx, CV_32F, 1, 0, 3);
        cv::Sobel(gray, gy, CV_32F, 0, 1, 3);
        cv::magnitude(gx, gy, mag);
        cv::max(mag, cv::Scalar(minContrast), den);
        cv::divide(gx, den, tx);
        cv::divide(gy, den, ty);
        // 软门限（内建）：连续斜坡 w = min(mag / minContrast, 1)，弱梯度给部分
        // 权重而非直接归零，轻微模糊/光照变化下分数不会塌到 0.4~0.7 区间。
        T w;
        cv::divide(mag, cv::Scalar(minContrast), w);
        cv::min(w, cv::Scalar(1.0), w);
        cv::multiply(tx, w, sx);
        cv::multiply(ty, w, sy);
    }

    //-------------------------------------------------------------------
    //  fused search image gradients (8U -> CV_16F, one pass per row)
    //-------------------------------------------------------------------
    // normGradients() above needs ten full image passes plus two CV_16F
    // conversions, and every pass reads and writes a 32F plane the size of the
    // image.  The same arithmetic is done here in one pass per row:
    //
    //   V[y][x] = g[y-1][x] + 2*g[y][x] + g[y+1][x]      (sobel smoothing)
    //   D[y][x] = g[y+1][x] - g[y-1][x]                  (sobel difference)
    //   gx = V[y][x+1] - V[y][x-1]      gy = D[y][x-1] + 2*D[y][x] + D[y][x+1]
    //   mag = sqrt(gx*gx + gy*gy),  den = max(mag, minContrast)
    //   out = (mag > minContrast) ? g / den : 0
    //
    // The sobel of an 8U image is an exact integer result, so the sequel is the
    // same float arithmetic as the OpenCV chain.  Verified against the old
    // chain: the halves are identical apart from at most a couple of pixels per
    // plane that differ by one ulp (cv::magnitude's rounding of the sqrt), which
    // the tolerance free scoring cannot see.  ~15x faster on 1280x1024.
    inline int reflect101(int i, int n)
    {
        if (i < 0)     return -i;
        if (i >= n)    return 2 * (n - 1) - i;
        return i;
    }

    void gradientHalves(const cv::Mat& gray8, cv::Mat& gx16, cv::Mat& gy16,
                        double minContrast, float w0, cv::Mat& q8, cv::Mat& m8)
    {
        CV_Assert(gray8.type() == CV_8U);
        const int h = gray8.rows, w = gray8.cols;
        gx16.create(h, w, CV_16F);
        gy16.create(h, w, CV_16F);
        // 8-bit mirror of the same normalised gradients.  Only ScanInvoker8
        // consumes it, and that kernel exists in an AVX2 build only -- so a
        // build without /arch:AVX2 must not pay for the mirror, otherwise the
        // scalar preprocess does the work twice for nothing (measured: 15.5 ->
        // 31.7 ms on a 32-bit build while the coarse scan gained nothing).
#if SM_USE_AVX2
        const bool wantMirror = true;
        q8.create(h, w, CV_8SC2);
        m8.create(h, w, CV_8U);
#else
        const bool wantMirror = false;
        q8.release();
        m8.release();
#endif

        const float mc = (float)minContrast;
        // a point may be scored when the sampled gradient length, i.e. the soft
        // weight, reaches w0 - exactly the test the scalar / half paths do
        // (|grad| >= w0).  w0 = 0 keeps everything; w0 > 1 keeps nothing, which
        // is also what the half path yields (|grad| <= 1), so do NOT clamp the
        // upper end: that would keep the |grad| == 1 pixels the half path drops.
        const float w0c = std::max(0.f, w0);
        const float w0sq = w0c * w0c;

        // Rows are independent: row y reads only gray8 rows y-1/ y / y+1 and
        // writes only its own gx16/gy16/q8/m8 row.  The one piece of per-row
        // state is the V/D scratch below, so it is allocated per band instead of
        // hoisted out of the loop.  This was the last fully serial stage of the
        // recognition path (measured 8.2 ms on a 2448x2048 image, i.e. 15% of
        // the whole search) - see .clinerules 7.4 item 8.
        auto processRows = [&](int y0, int y1)
        {
            // one padding element on each side holds the BORDER_REFLECT_101
            // value, so the inner loop never needs a branch for x == 0 / x == w-1
            std::vector<short> vbuf((size_t)w + 16), dbuf((size_t)w + 16);
            short* V = vbuf.data() + 1;
            short* D = dbuf.data() + 1;
#if SM_USE_AVX2
            const __m256 vmc = _mm256_set1_ps(mc);
            const __m256 one = _mm256_set1_ps(1.f);
            const __m256 vW02 = _mm256_set1_ps(w0sq);
            const __m256 v127 = _mm256_set1_ps((float)kQ8);
            const __m256i vOne32 = _mm256_set1_epi32(1);
#endif

        for (int y = y0; y < y1; y++)
        {
            const uchar* r0 = gray8.ptr<uchar>(reflect101(y - 1, h));
            const uchar* r1 = gray8.ptr<uchar>(y);
            const uchar* r2 = gray8.ptr<uchar>(reflect101(y + 1, h));

            // vertical smoothing / difference of the three source rows
            int x = 0;
#if SM_USE_AVX2
            for (; x + 16 <= w; x += 16)
            {
                const __m256i a = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i*)(r0 + x)));
                const __m256i b = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i*)(r1 + x)));
                const __m256i c = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i*)(r2 + x)));
                _mm256_storeu_si256((__m256i*)(V + x),
                                    _mm256_add_epi16(_mm256_add_epi16(a, c),
                                                     _mm256_slli_epi16(b, 1)));
                _mm256_storeu_si256((__m256i*)(D + x), _mm256_sub_epi16(c, a));
            }
#endif
            for (; x < w; x++)
            {
                V[x] = (short)(r0[x] + 2 * r1[x] + r2[x]);
                D[x] = (short)((int)r2[x] - (int)r0[x]);
            }
            V[-1] = V[1]; V[w] = V[w - 2];
            D[-1] = D[1]; D[w] = D[w - 2];

            // horizontal difference / smoothing + normalization + half store
            unsigned short* gxr = gx16.ptr<unsigned short>(y);
            unsigned short* gyr = gy16.ptr<unsigned short>(y);
            signed char* qr = wantMirror ? q8.ptr<signed char>(y) : nullptr;
            uchar* mr = wantMirror ? m8.ptr<uchar>(y) : nullptr;
            x = 0;
#if SM_USE_AVX2
            for (; x + 8 <= w; x += 8)
            {
                // sobel x: V[x+1] - V[x-1],  sobel y: D[x-1] + 2*D[x] + D[x+1]
                // both stay inside +-1020, so the integer difference is exact
                const __m128i dcl = _mm_loadu_si128((const __m128i*)(D + x));
                const __m128i gxi = _mm_sub_epi16(
                    _mm_loadu_si128((const __m128i*)(V + x + 1)),
                    _mm_loadu_si128((const __m128i*)(V + x - 1)));
                const __m128i gyi = _mm_add_epi16(
                    _mm_add_epi16(_mm_loadu_si128((const __m128i*)(D + x - 1)),
                                  _mm_loadu_si128((const __m128i*)(D + x + 1))),
                    _mm_add_epi16(dcl, dcl));
                const __m256 gx = _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(gxi));
                const __m256 gy = _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(gyi));
                const __m256 mag = _mm256_sqrt_ps(
                    _mm256_add_ps(_mm256_mul_ps(gx, gx), _mm256_mul_ps(gy, gy)));
                const __m256 den = _mm256_max_ps(mag, vmc);
                // 软门限（内建）：连续斜坡 min(mag/mc,1)，弱梯度给部分权重而非归零
                const __m256 wt = _mm256_min_ps(_mm256_div_ps(mag, vmc), one);
                const __m256 ox = _mm256_mul_ps(_mm256_div_ps(gx, den), wt);
                const __m256 oy = _mm256_mul_ps(_mm256_div_ps(gy, den), wt);
                const __m128i hx = _mm256_cvtps_ph(ox, _MM_FROUND_TO_NEAREST_INT);
                const __m128i hy = _mm256_cvtps_ph(oy, _MM_FROUND_TO_NEAREST_INT);
                _mm_storeu_si128((__m128i*)(gxr + x), hx);
                _mm_storeu_si128((__m128i*)(gyr + x), hy);

                // ---- 8-bit mirror: mask = |half grad| >= w0 (same test the
                // half paths run on the sampled values) + a 1/0 count plane.
                const __m256 fx = _mm256_cvtph_ps(hx);
                const __m256 fy = _mm256_cvtph_ps(hy);
                const __m256i keep = _mm256_castps_si256(_mm256_cmp_ps(
                    _mm256_add_ps(_mm256_mul_ps(fx, fx), _mm256_mul_ps(fy, fy)),
                    vW02, _CMP_GE_OQ));
                __m256i qxi = _mm256_cvtps_epi32(_mm256_mul_ps(fx, v127));
                __m256i qyi = _mm256_cvtps_epi32(_mm256_mul_ps(fy, v127));
                qxi = _mm256_and_si256(qxi, keep);
                qyi = _mm256_and_si256(qyi, keep);
                // interleave (qx,qy) per pixel: 32 -> 16 -> 8 bit, one 16B store
                const __m256i il = _mm256_unpacklo_epi32(qxi, qyi);
                const __m256i ih = _mm256_unpackhi_epi32(qxi, qyi);
                const __m256i i16 = _mm256_packs_epi32(il, ih);
                _mm_storeu_si128((__m128i*)(qr + 2 * x),
                                 _mm_packs_epi16(_mm256_castsi256_si128(i16),
                                                 _mm256_extracti128_si256(i16, 1)));
                const __m256i m1 = _mm256_and_si256(_mm256_srli_epi32(keep, 31), vOne32);
                const __m128i m16 = _mm_packs_epi32(_mm256_castsi256_si128(m1),
                                                    _mm256_extracti128_si256(m1, 1));
                _mm_storel_epi64((__m128i*)(mr + x), _mm_packs_epi16(m16, m16));
            }
#endif
            // tail (and the whole row on a non-AVX2 build): same arithmetic,
            // one pixel at a time, so the 8-bit mirror needs no special case
            for (; x < w; x++)
            {
                const float gx = (float)(V[x + 1] - V[x - 1]);
                const float gy = (float)(D[x - 1] + 2 * D[x] + D[x + 1]);
                const float magf = std::sqrt(gx * gx + gy * gy);
                const float denf = std::max(magf, mc);
                const float mf = std::min(magf / mc, 1.f);
                float ox = gx / denf * mf;
                float oy = gy / denf * mf;
#if SM_USE_AVX2
                // keep the 8-bit mirror identical to the vector loop: it sees
                // the half-rounded value, not the float one
                const __m128i hx1 = _mm256_cvtps_ph(_mm256_set1_ps(ox), _MM_FROUND_TO_NEAREST_INT);
                const __m128i hy1 = _mm256_cvtps_ph(_mm256_set1_ps(oy), _MM_FROUND_TO_NEAREST_INT);
                gxr[x] = (unsigned short)_mm_extract_epi16(hx1, 0);
                gyr[x] = (unsigned short)_mm_extract_epi16(hy1, 0);
                ox = _mm_cvtss_f32(_mm_cvtph_ps(hx1));
                oy = _mm_cvtss_f32(_mm_cvtph_ps(hy1));
#else
                gxr[x] = f32ToF16(ox);
                gyr[x] = f32ToF16(oy);
#endif
                // the mirror is only built in an AVX2 build (see wantMirror)
                if (wantMirror)
                {
                    const bool keep = (ox * ox + oy * oy) >= w0sq;
                    qr[2 * x]     = keep ? (signed char)cvRound(ox * (float)kQ8) : (signed char)0;
                    qr[2 * x + 1] = keep ? (signed char)cvRound(oy * (float)kQ8) : (signed char)0;
                    mr[x] = keep ? (uchar)1 : (uchar)0;
                }
            }
        }
        };   // end processRows

        // Small images (the coarse pyramid levels) are not worth the dispatch
        // overhead: below ~200k pixels the band count would exceed the rows per
        // thread anyway.  SM_PREP_SERIAL=1 forces the serial path for A/B.
        static const bool prepSerial = (std::getenv("SM_PREP_SERIAL") != nullptr);
        const long long px = (long long)w * (long long)h;
        if (!prepSerial && px >= 200000 && cv::getNumThreads() > 1)
        {
            // One band per ~32 rows: with 20 threads on a 2048-row level that is
            // 64 stripes (3+ per thread), which balances better than 64-row
            // bands (32 stripes, only 1.6 per thread).  SM_PREP_BAND overrides
            // the band height for tuning.
            static const int bandRows = []
            {
                const char* v = std::getenv("SM_PREP_BAND");
                const int b = v ? std::atoi(v) : 0;
                return (b > 0) ? b : 32;
            }();
            const double bands = std::max(2.0, std::min((double)h / (double)bandRows,
                                                        (double)cv::getNumThreads() * 4.0));
            cv::parallel_for_(cv::Range(0, h),
                              [&](const cv::Range& r) { processRows(r.start, r.end); },
                              bands);
        }
        else
        {
            processRows(0, h);
        }
    }

    // fallback for the levels the fused pass cannot handle (OpenCL build or an
    // image too small for the padded rows): derive the 8-bit mirror from the
    // already normalised halves, with exactly the same rule
    //     keep = |half grad| >= w0,   q = round(half grad * 127)
    void quantizeHalves(const cv::Mat& gx16, const cv::Mat& gy16, float w0,
                        cv::Mat& q8, cv::Mat& m8)
    {
        const int h = gx16.rows, w = gx16.cols;
        q8.create(h, w, CV_8SC2);
        m8.create(h, w, CV_8U);
        // same rule as the fused pass in gradientHalves: no upper clamp
        const float w0c = std::max(0.f, w0);
        const float w0sq = w0c * w0c;
#if SM_USE_AVX2
        const __m256 vW02 = _mm256_set1_ps(w0sq);
        const __m256 v127 = _mm256_set1_ps((float)kQ8);
        const __m256i vOne32 = _mm256_set1_epi32(1);
#endif
        for (int y = 0; y < h; y++)
        {
            const unsigned short* xr = gx16.ptr<unsigned short>(y);
            const unsigned short* yr = gy16.ptr<unsigned short>(y);
            signed char* qr = q8.ptr<signed char>(y);
            uchar* mr = m8.ptr<uchar>(y);
            int x = 0;
#if SM_USE_AVX2
            for (; x + 8 <= w; x += 8)
            {
                const __m256 fx = _mm256_cvtph_ps(_mm_loadu_si128((const __m128i*)(xr + x)));
                const __m256 fy = _mm256_cvtph_ps(_mm_loadu_si128((const __m128i*)(yr + x)));
                const __m256i keep = _mm256_castps_si256(_mm256_cmp_ps(
                    _mm256_add_ps(_mm256_mul_ps(fx, fx), _mm256_mul_ps(fy, fy)),
                    vW02, _CMP_GE_OQ));
                __m256i qxi = _mm256_cvtps_epi32(_mm256_mul_ps(fx, v127));
                __m256i qyi = _mm256_cvtps_epi32(_mm256_mul_ps(fy, v127));
                qxi = _mm256_and_si256(qxi, keep);
                qyi = _mm256_and_si256(qyi, keep);
                const __m256i i16 = _mm256_packs_epi32(_mm256_unpacklo_epi32(qxi, qyi),
                                                       _mm256_unpackhi_epi32(qxi, qyi));
                _mm_storeu_si128((__m128i*)(qr + 2 * x),
                                 _mm_packs_epi16(_mm256_castsi256_si128(i16),
                                                 _mm256_extracti128_si256(i16, 1)));
                const __m256i m1 = _mm256_and_si256(_mm256_srli_epi32(keep, 31), vOne32);
                const __m128i m16 = _mm_packs_epi32(_mm256_castsi256_si128(m1),
                                                    _mm256_extracti128_si256(m1, 1));
                _mm_storel_epi64((__m128i*)(mr + x), _mm_packs_epi16(m16, m16));
            }
#endif
            for (; x < w; x++)
            {
                const float ox = f16ToF32(xr[x]);
                const float oy = f16ToF32(yr[x]);
                const bool keep = (ox * ox + oy * oy) >= w0sq;
                qr[2 * x]     = keep ? (signed char)cvRound(ox * (float)kQ8) : (signed char)0;
                qr[2 * x + 1] = keep ? (signed char)cvRound(oy * (float)kQ8) : (signed char)0;
                mr[x] = keep ? (uchar)1 : (uchar)0;
            }
        }
    }
}

//=======================================================================
//  correlation of the whole level (dense, optionally on OpenCL)
//=======================================================================
void ShapeMatcher::correlate(int level, int aIdx, int sIdx, cv::Mat& scoreMap, int& R)
{
    XForm& xf = getXForm(level, aIdx, sIdx);
    if (!xf.hasKernel)
        buildKernel(xf);
    R = xf.R;

    // Same guard as correlateSparseX: filter2D below would be fine, but the crop
    // uses (rows - 2R) which goes negative once the kernel outgrows the level.
    if (2 * R + 1 > srcGx_[level].rows || 2 * R + 1 > srcGx_[level].cols)
    {
        scoreMap.release();
        return;
    }

    const float invN = 1.f / std::max(1, xf.n);
    if (useOCL_)
    {
        cv::UMat ku1, ku2;
        xf.kx.copyTo(ku1);
        xf.ky.copyTo(ku2);
        cv::UMat c1, c2, sum;
        cv::filter2D(uGx_[level], c1, CV_32F, ku1);
        cv::filter2D(uGy_[level], c2, CV_32F, ku2);
        cv::add(c1, c2, sum);
        cv::UMat crop(sum, cv::Rect(R, R, sum.cols - 2 * R, sum.rows - 2 * R));
        crop.copyTo(scoreMap);
        scoreMap *= invN;
    }
    else
    {
        // the gradients are kept as halves; filter2D needs 32F, so unpack
        cv::Mat gx32, gy32, c1, c2, sum;
        srcGx_[level].convertTo(gx32, CV_32F);
        srcGy_[level].convertTo(gy32, CV_32F);
        cv::filter2D(gx32, c1, CV_32F, xf.kx);
        cv::filter2D(gy32, c2, CV_32F, xf.ky);
        cv::add(c1, c2, sum);
        sum(cv::Rect(R, R, sum.cols - 2 * R, sum.rows - 2 * R)).copyTo(scoreMap);
        scoreMap *= invN;
    }
}

//=======================================================================
//  sparse scan of the whole level (multithreaded, with greedy early exit)
//=======================================================================
namespace
{
    class ScanInvoker : public cv::ParallelLoopBody
    {
    public:
        const cv::Mat* sx;
        const cv::Mat* sy;
        const XForm* xf;
        cv::Mat* out;
        float target;   // min score to survive
        float slope;    // greediness slope
        float w0;       // skip weight: model points whose sampled gradient
                        // length (= min(|grad|/minContrast,1)) is below w0
                        // are excluded from the mean (kept count in the
                        // denominator instead of n).  0 = off.

        ScanInvoker(const cv::Mat& _sx, const cv::Mat& _sy, const XForm& _xf,
                    cv::Mat& _out, float _target, float _slope, float _w0 = 0.f)
            : sx(&_sx), sy(&_sy), xf(&_xf), out(&_out), target(_target), slope(_slope), w0(_w0) {}

        void operator()(const cv::Range& range) const CV_OVERRIDE
        {
            const int R = xf->R;
            const int n = xf->n;
            const int K = 2 * R + 1;
            const int w = sx->cols;
            const int16_t* SM_RESTRICT pox = xf->ox.data();
            const int16_t* SM_RESTRICT poy = xf->oy.data();
            const float*   SM_RESTRICT pnx = xf->nx.data();
            const float*   SM_RESTRICT pny = xf->ny.data();

            // skipWeight bookkeeping: a position is only believable when a
            // decent fraction of the model points actually has usable gradient
            // magnitude in the search image, otherwise the division by the
            // kept count could blow noise up to a "perfect" score.
            const int   minKept = std::max(8, n / 4);
            const float invKept = 1.f / (float)minKept;

            // the gradient images are CV_16F (half) -> half the memory traffic
            std::vector<const uint16_t*> rx(K), ry(K);
            const float invN = 1.f / n;
            const int xEnd = w - R;

            for (int yi = range.start; yi < range.end; yi++)
            {
                const int y = yi + R;
                for (int i = 0; i < K; i++)
                {
                    rx[i] = sx->ptr<uint16_t>(y - R + i);
                    ry[i] = sy->ptr<uint16_t>(y - R + i);
                }
                float* SM_RESTRICT orow = out->ptr<float>(yi);

                int x = R;
#if SM_USE_AVX2
                const __m256 vInvN = _mm256_set1_ps(invN);
                const __m256 vDead = _mm256_set1_ps(-1.f);
                const __m256 vOne  = _mm256_set1_ps(1.f);
                const __m256 vW02  = _mm256_set1_ps(w0 * w0);
                const __m256 vMinK = _mm256_set1_ps((float)minKept);
                // 16 positions per iteration: two 256-bit float accumulators.
                // Each gather then touches 16 halves = 32 B, twice as much of
                // the 64 B cache line as the 8-wide variant.
                //
                // skipWeight: per point the sampled gradient length equals the
                // soft weight (w = min(|grad|/minContrast, 1)), so
                // len2 = a*a + b*b < w0*w0 marks a point that Halcon would not
                // score at all.  Its dot and its "1" in the denominator are
                // masked off; the final mean divides by the kept count (with a
                // minKept floor, below which the position is unreliable and
                // reported dead).  With w0 = 0 the mask is all-ones and this
                // reduces to the previous sum * invN behaviour bit for bit.
                for (; x + 16 <= xEnd; x += 16)
                {
                    __m256 sum0 = _mm256_setzero_ps();
                    __m256 sum1 = _mm256_setzero_ps();
                    __m256 cnt0 = _mm256_setzero_ps();
                    __m256 cnt1 = _mm256_setzero_ps();
                    bool dead0 = false, dead1 = false;
                    for (int k = 0; k < n; k++)
                    {
                        const int ix = x + pox[k];
                        const int ri = poy[k] + R;
                        const uint16_t* pA = rx[ri] + ix;
                        const uint16_t* pB = ry[ri] + ix;
                        // 16 halves = 256 bit
                        __m256i ha = _mm256_loadu_si256((const __m256i*)pA);
                        __m256i hb = _mm256_loadu_si256((const __m256i*)pB);
                        // touch the next model point's row while we wait
                        if (k + 1 < n)
                        {
                            _mm_prefetch((const char*)(rx[poy[k + 1] + R] + x + pox[k + 1]), _MM_HINT_T0);
                            _mm_prefetch((const char*)(ry[poy[k + 1] + R] + x + pox[k + 1]), _MM_HINT_T0);
                        }
                        __m256 na = _mm256_set1_ps(pnx[k]);
                        __m256 nb = _mm256_set1_ps(pny[k]);
                        if (!dead0)
                        {
                            __m256 a0 = _mm256_cvtph_ps(_mm256_castsi256_si128(ha));
                            __m256 b0 = _mm256_cvtph_ps(_mm256_castsi256_si128(hb));
                            __m256 d0 = _mm256_add_ps(_mm256_mul_ps(na, a0), _mm256_mul_ps(nb, b0));
                            __m256 m0 = _mm256_cmp_ps(
                                _mm256_add_ps(_mm256_mul_ps(a0, a0), _mm256_mul_ps(b0, b0)),
                                vW02, _CMP_GE_OQ);                 // all-ones where strong
                            sum0 = _mm256_add_ps(sum0, _mm256_and_ps(d0, m0));
                            cnt0 = _mm256_add_ps(cnt0, _mm256_and_ps(vOne, m0));
                        }
                        if (!dead1)
                        {
                            __m256 a1 = _mm256_cvtph_ps(_mm256_extracti128_si256(ha, 1));
                            __m256 b1 = _mm256_cvtph_ps(_mm256_extracti128_si256(hb, 1));
                            __m256 d1 = _mm256_add_ps(_mm256_mul_ps(na, a1), _mm256_mul_ps(nb, b1));
                            __m256 m1 = _mm256_cmp_ps(
                                _mm256_add_ps(_mm256_mul_ps(a1, a1), _mm256_mul_ps(b1, b1)),
                                vW02, _CMP_GE_OQ);
                            sum1 = _mm256_add_ps(sum1, _mm256_and_ps(d1, m1));
                            cnt1 = _mm256_add_ps(cnt1, _mm256_and_ps(vOne, m1));
                        }
                        int m = k + 1;
                        if (((m & 7) == 0) || m == n)
                        {
                            float x1 = (target - 1.f) + slope * m;
                            float x2 = target * invN * m;
                            float lim = (x1 < x2 ? x1 : x2) * m;
                            __m256 vlim = _mm256_set1_ps(lim);
                            if (!dead0 && _mm256_movemask_ps(_mm256_cmp_ps(sum0, vlim, _CMP_LT_OQ)) == 0xFF)
                                dead0 = true;
                            if (!dead1 && _mm256_movemask_ps(_mm256_cmp_ps(sum1, vlim, _CMP_LT_OQ)) == 0xFF)
                                dead1 = true;
                            if (dead0 && dead1)
                                break;
                        }
                    }
                    // mean over the kept points; below minKept the position is
                    // not believable -> dead
                    __m256 ok0 = _mm256_cmp_ps(cnt0, vMinK, _CMP_GE_OQ);
                    __m256 sc0 = _mm256_div_ps(sum0, cnt0);
                    _mm256_storeu_ps(orow + x - R,
                                     dead0 ? vDead : _mm256_blendv_ps(vDead, sc0, ok0));
                    __m256 ok1 = _mm256_cmp_ps(cnt1, vMinK, _CMP_GE_OQ);
                    __m256 sc1 = _mm256_div_ps(sum1, cnt1);
                    _mm256_storeu_ps(orow + x - R + 8,
                                     dead1 ? vDead : _mm256_blendv_ps(vDead, sc1, ok1));
                }
                // eight neighbouring positions share the same offsets, so the
                // eight source values are consecutive in memory -> one load
                for (; x + 8 <= xEnd; x += 8)
                {
                    __m256 sum = _mm256_setzero_ps();
                    __m256 cnt = _mm256_setzero_ps();
                    bool dead = false;
                    for (int k = 0; k < n; k++)
                    {
                        const int ix = x + pox[k];
                        const int ri = poy[k] + R;
                        const uint16_t* pA = rx[ri] + ix;
                        const uint16_t* pB = ry[ri] + ix;
                        // 8 halves = 128 bit -> 8 floats
                        __m256 a = _mm256_cvtph_ps(_mm_loadu_si128((const __m128i*)pA));
                        __m256 b = _mm256_cvtph_ps(_mm_loadu_si128((const __m128i*)pB));
                        __m256 d8 = _mm256_add_ps(
                            _mm256_mul_ps(_mm256_set1_ps(pnx[k]), a),
                            _mm256_mul_ps(_mm256_set1_ps(pny[k]), b));
                        __m256 mk8 = _mm256_cmp_ps(
                            _mm256_add_ps(_mm256_mul_ps(a, a), _mm256_mul_ps(b, b)),
                            vW02, _CMP_GE_OQ);
                        sum = _mm256_add_ps(sum, _mm256_and_ps(d8, mk8));
                        cnt = _mm256_add_ps(cnt, _mm256_and_ps(vOne, mk8));
                        int m = k + 1;
                        if (((m & 7) == 0) || m == n)
                        {
                            float x1 = (target - 1.f) + slope * m;
                            float x2 = target * invN * m;
                            float lim = (x1 < x2 ? x1 : x2) * m;
                            if (_mm256_movemask_ps(_mm256_cmp_ps(sum, _mm256_set1_ps(lim), _CMP_LT_OQ)) == 0xFF)
                            {
                                dead = true;
                                break;
                            }
                        }
                    }
                    if (dead)
                        _mm256_storeu_ps(orow + x - R, vDead);
                    else
                    {
                        __m256 ok8 = _mm256_cmp_ps(cnt, vMinK, _CMP_GE_OQ);
                        _mm256_storeu_ps(orow + x - R,
                                         _mm256_blendv_ps(vDead,
                                                          _mm256_div_ps(sum, cnt), ok8));
                    }
                }
#endif
#if SM_USE_SSE2
                // four neighbouring positions share the same offsets, so the
                // four source values are consecutive in memory -> one vector load
                const __m128 vInvN128 = _mm_set1_ps(invN);
                const __m128 vOne128  = _mm_set1_ps(1.f);
                const __m128 vW02128  = _mm_set1_ps(w0 * w0);
                const __m128 vMinK128 = _mm_set1_ps((float)minKept);
                const __m128 vDead128 = _mm_set1_ps(-1.f);
                for (; x + 4 <= xEnd; x += 4)
                {
                    __m128 sum = _mm_setzero_ps();
                    __m128 cnt = _mm_setzero_ps();
                    bool dead = false;
                    for (int k = 0; k < n; k++)
                    {
                        const int ix = x + pox[k];
                        const int ri = poy[k] + R;
                        _mm_prefetch((const char*)(rx[ri] + ix + 16), _MM_HINT_T0);
                        _mm_prefetch((const char*)(ry[ri] + ix + 16), _MM_HINT_T0);
                        // 4 halves = 64 bit -> 4 floats
                        __m128 a = _mm_cvtph_ps(_mm_loadl_epi64((const __m128i*)(rx[ri] + ix)));
                        __m128 b = _mm_cvtph_ps(_mm_loadl_epi64((const __m128i*)(ry[ri] + ix)));
                        __m128 d4 = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(pnx[k]), a),
                                               _mm_mul_ps(_mm_set1_ps(pny[k]), b));
                        __m128 mk4 = _mm_cmpge_ps(
                            _mm_add_ps(_mm_mul_ps(a, a), _mm_mul_ps(b, b)), vW02128);
                        sum = _mm_add_ps(sum, _mm_and_ps(d4, mk4));
                        cnt = _mm_add_ps(cnt, _mm_and_ps(vOne128, mk4));
                        int m = k + 1;
                        if (((m & 3) == 0) || m == n)
                        {
                            float x1 = (target - 1.f) + slope * m;
                            float x2 = target * invN * m;
                            float lim = (x1 < x2 ? x1 : x2) * m;
                            if (_mm_movemask_ps(_mm_cmplt_ps(sum, _mm_set1_ps(lim))) == 15)
                            {
                                dead = true;
                                break;
                            }
                        }
                    }
                    if (dead)
                    {
                        _mm_storeu_ps(orow + x - R, vDead128);
                    }
                    else
                    {
                        // SSE2 has no blendv: or(and(score, ok), andnot(ok, dead))
                        __m128 ok4 = _mm_cmpge_ps(cnt, vMinK128);
                        __m128 sc4 = _mm_div_ps(sum, cnt);
                        _mm_storeu_ps(orow + x - R,
                                      _mm_or_ps(_mm_and_ps(sc4, ok4),
                                                _mm_andnot_ps(ok4, vDead128)));
                    }
                }
#endif
                for (; x < xEnd; x++)
                {
                    float sum = 0.f;
                    int   cnt = 0;
                    bool dead = false;
                    for (int k = 0; k < n; k++)
                    {
                        int ix = x + pox[k];
                        int ri = poy[k] + R;
                        float ga = f16ToF32(rx[ri][ix]);
                        float gb = f16ToF32(ry[ri][ix]);
                        if (ga * ga + gb * gb >= w0 * w0)
                        {
                            sum += pnx[k] * ga + pny[k] * gb;
                            cnt++;
                        }
                        int m = k + 1;
                        if (((m & 3) == 0) || m == n)
                        {
                            // greedy stop: the remaining points can no longer lift
                            // the score above the required minimum
                            float x1 = (target - 1.f) + slope * m;
                            float x2 = target * invN * m;
                            float lim = (x1 < x2 ? x1 : x2) * m;
                            if (sum < lim)
                            {
                                dead = true;
                                break;
                            }
                        }
                    }
                    orow[x - R] = (dead || cnt < minKept) ? -1.f
                                                          : (cnt > 0 ? sum / cnt : -1.f);
                }
            }
        }
    };

    //-------------------------------------------------------------------
    //  Integer coarse scan ("8 bit" kernel).
    //
    //  Same greedy early exit as ScanInvoker, but the inner loop is integer
    //  only:
    //    gradient  CV_8SC2 (qx,qy)   q  = round(grad*127)   |grad| <= 1
    //    normal    int32 packed       nq = round(n*258)      |n|    = 1
    //    count     CV_8U 1/0         (skipWeight already folded in)
    //  Per model point and per 8 positions that is
    //    vpmovsxbw (16 interleaved bytes -> 16 int16)
    //    vpmaddwd (8 int32 dots) + vpaddd
    //    vpmovsxbw + vpaddw (mask bytes -> 16 kept counts)
    //  ~5 uops instead of 2 vcvtph2ps + 6 mul/add + 2 cmp/and in the half
    //  path, and the two planes are 3 bytes/pixel instead of 4.
    //
    //  Result = sum / (127*258 * keptCount), i.e. the same score quantised to
    //  8 bit (difference ~2e-3).  It is therefore only used for the coarse
    //  ranking: every reported score is recomputed from the halves
    //  (refine / subpixel / rescore), so no output value depends on it.
    //-------------------------------------------------------------------
    class ScanInvoker8 : public cv::ParallelLoopBody
    {
    public:
        const cv::Mat* q8;   // CV_8SC2, 0 where the point may not be scored
        const cv::Mat* m8;   // CV_8U, 1 where the point may be scored
        const XForm* xf;
        cv::Mat* out;
        float target;        // min score to survive
        float slope;         // greediness slope

        ScanInvoker8(const cv::Mat& _q8, const cv::Mat& _m8, const XForm& _xf,
                     cv::Mat& _out, float _target, float _slope)
            : q8(&_q8), m8(&_m8), xf(&_xf), out(&_out), target(_target), slope(_slope) {}

        void operator()(const cv::Range& range) const CV_OVERRIDE
        {
            const int R = xf->R;
            const int n = xf->n;
            const int K = 2 * R + 1;
            const int w = q8->cols;
            const int16_t* SM_RESTRICT pox = xf->ox.data();
            const int16_t* SM_RESTRICT poy = xf->oy.data();
            const int32_t* SM_RESTRICT pnp = xf->np.data();

            const int   minKept = std::max(8, n / 4);
            const float invN = 1.f / n;
            const int xEnd = w - R;
            const float qdot = kQ8Dot;

            std::vector<const signed char*> qr(K);
            std::vector<const uchar*>       mr(K);
            // the interleaved plane is addressed in bytes: twice the pixel offset
            std::vector<int> qoff(n), moff(n);
            for (int k = 0; k < n; k++)
            {
                qoff[k] = 2 * (int)pox[k];
                moff[k] = (int)pox[k];
            }

            for (int yi = range.start; yi < range.end; yi++)
            {
                const int y = yi + R;
                for (int i = 0; i < K; i++)
                {
                    qr[i] = q8->ptr<signed char>(y - R + i);
                    mr[i] = m8->ptr<uchar>(y - R + i);
                }
                float* SM_RESTRICT orow = out->ptr<float>(yi);
                int x = R;
                // ---- AVX2 body: 16 positions per iteration ----------------
#if SM_USE_AVX2
                const __m256 vQDot = _mm256_set1_ps(qdot);
                const __m256 vDead = _mm256_set1_ps(-1.f);
                const __m256 vMinK = _mm256_set1_ps((float)minKept);
                // 16 positions per iteration: two int32 accumulators for the
                // dots (8 positions each) and one int16 accumulator that holds
                // the kept count of all 16 positions at once.
                for (; x + 16 <= xEnd; x += 16)
                {
                    __m256i sum0 = _mm256_setzero_si256();
                    __m256i sum1 = _mm256_setzero_si256();
                    __m256i cnt  = _mm256_setzero_si256();
                    bool dead0 = false, dead1 = false;
                    for (int k = 0; k < n; k++)
                    {
                        const int ri = poy[k] + R;
                        const signed char* pq = qr[ri] + 2 * x + qoff[k];
                        const uchar*       pm = mr[ri] + x + moff[k];
                        const __m256i vn = _mm256_set1_epi32(pnp[k]);
                        if (!dead0)
                        {
                            const __m256i g = _mm256_cvtepi8_epi16(
                                _mm_loadu_si128((const __m128i*)pq));
                            sum0 = _mm256_add_epi32(sum0, _mm256_madd_epi16(g, vn));
                        }
                        if (!dead1)
                        {
                            const __m256i g = _mm256_cvtepi8_epi16(
                                _mm_loadu_si128((const __m128i*)(pq + 16)));
                            sum1 = _mm256_add_epi32(sum1, _mm256_madd_epi16(g, vn));
                        }
                        // each point adds 0/1 to every position's kept count
                        cnt = _mm256_add_epi16(cnt, _mm256_cvtepi8_epi16(
                                  _mm_loadu_si128((const __m128i*)pm)));
                        int m = k + 1;
                        if (((m & 7) == 0) || m == n)
                        {
                            float x1 = (target - 1.f) + slope * m;
                            float x2 = target * invN * m;
                            float lim = (x1 < x2 ? x1 : x2) * m;
                            // the accumulator holds the score scaled by 127*258,
                            // so the bound moves to that scale as well (|lim| and
                            // its product stay far inside int32)
                            const __m256i vlim = _mm256_set1_epi32((int)(lim * qdot));
                            if (!dead0 && _mm256_movemask_ps(_mm256_castsi256_ps(
                                    _mm256_cmpgt_epi32(vlim, sum0))) == 0xFF)
                                dead0 = true;
                            if (!dead1 && _mm256_movemask_ps(_mm256_castsi256_ps(
                                    _mm256_cmpgt_epi32(vlim, sum1))) == 0xFF)
                                dead1 = true;
                            if (dead0 && dead1)
                                break;
                        }
                    }
                    const __m256 fc0 = _mm256_cvtepi32_ps(
                        _mm256_cvtepi16_epi32(_mm256_castsi256_si128(cnt)));
                    const __m256 fc1 = _mm256_cvtepi32_ps(
                        _mm256_cvtepi16_epi32(_mm256_extracti128_si256(cnt, 1)));
                    const __m256 ok0 = _mm256_cmp_ps(fc0, vMinK, _CMP_GE_OQ);
                    const __m256 ok1 = _mm256_cmp_ps(fc1, vMinK, _CMP_GE_OQ);
                    const __m256 sc0 = _mm256_div_ps(_mm256_cvtepi32_ps(sum0),
                                                     _mm256_mul_ps(fc0, vQDot));
                    const __m256 sc1 = _mm256_div_ps(_mm256_cvtepi32_ps(sum1),
                                                     _mm256_mul_ps(fc1, vQDot));
                    _mm256_storeu_ps(orow + x - R,
                                     dead0 ? vDead : _mm256_blendv_ps(vDead, sc0, ok0));
                    _mm256_storeu_ps(orow + x - R + 8,
                                     dead1 ? vDead : _mm256_blendv_ps(vDead, sc1, ok1));
                }
#endif
                // ---- tail, and the whole row on a non-AVX2 build ----
                for (; x < xEnd; x++)
                {
                    int  sum = 0;
                    int  cnt = 0;
                    bool dead = false;
                    for (int k = 0; k < n; k++)
                    {
                        const int ri = poy[k] + R;
                        const signed char* pq = qr[ri] + 2 * (x + pox[k]);
                        const int32_t packed = pnp[k];
                        const int32_t nqx = (int16_t)(packed & 0xFFFF);
                        const int32_t nqy = (int16_t)(packed >> 16);
                        sum += (int)pq[0] * nqx + (int)pq[1] * nqy;
                        cnt += (int)mr[ri][x + pox[k]];
                        int m = k + 1;
                        if (((m & 7) == 0) || m == n)
                        {
                            float x1 = (target - 1.f) + slope * m;
                            float x2 = target * invN * m;
                            float lim = (x1 < x2 ? x1 : x2) * m;
                            if ((float)sum < lim * qdot)
                            {
                                dead = true;
                                break;
                            }
                        }
                    }
                    orow[x - R] = (dead || cnt < minKept)
                                      ? -1.f
                                      : (float)sum / (qdot * (float)cnt);
                }
            }
        }
    };
}

void ShapeMatcher::correlateSparse(int level, int aIdx, int sIdx, cv::Mat& scoreMap,
                                   int& R, double target, double greed, bool nested)
{
    correlateSparseX(level, getXForm(level, aIdx, sIdx), scoreMap, R, target, greed, nested);
}

void ShapeMatcher::correlateSparseX(int level, const XForm& xf, cv::Mat& scoreMap,
                                    int& R, double target, double greed, bool nested)
{
    R = xf.R;
    const int K = 2 * R + 1;
    CV_Assert(srcGx_[level].type() == CV_16F && srcGy_[level].type() == CV_16F);
    // A pose whose correlation kernel is bigger than the (pyramid) search image
    // has no valid position at all.  cv::Mat::create would abort on the negative
    // size (Assertion s >= 0 in cv::setSize), which makes a too wide scale range
    // or an oversized ROI kill the whole process instead of just finding nothing.
    // An empty map is treated as "no candidate" by every consumer
    // (extractCandidates / minMaxSerial both tolerate empty input).
    if (K > srcGx_[level].rows || K > srcGx_[level].cols)
    {
        scoreMap.release();
        return;
    }
    scoreMap.create(srcGx_[level].rows - K + 1, srcGx_[level].cols - K + 1, CV_32F);

    double g = std::min(std::max(greed, 0.0), 0.98);
    float slope = (float)((1.0 - g * target) / ((1.0 - g) * std::max(1, xf.n)));

    // The 8-bit kernel is only a ranking shortcut: it must produce the same
    // local maxima as the half path, which is why it is enabled only when the
    // quantised planes exist for this level and the point count keeps the
    // int32 accumulator (n*32766) and the int16 kept-count (n) in range.
    // SM_NO_Q8=1 in the environment forces the half path, so the two kernels
    // can be compared on the same data (results must match to ~2e-3).
    static const bool q8Disabled = (std::getenv("SM_NO_Q8") != nullptr);
    const bool useQ8 = !q8Disabled && SM_USE_AVX2 && xf.n > 0 && xf.n <= kQ8MaxPoints &&
                       (int)xf.np.size() == xf.n &&
                       level < (int)srcQ8_.size() && !srcQ8_[level].empty() &&
                       !srcM8_[level].empty() &&
                       srcM8_[level].rows == srcGx_[level].rows &&
                       srcM8_[level].cols == srcGx_[level].cols;
    // when we are already inside a parallel loop the rows must be scanned
    // serially: nested parallel_for_ corrupts the OpenCV thread pool
    double stripes = nested ? 1.0 : (double)std::max(1, scoreMap.rows / 4);

    if (useQ8)
    {
        ScanInvoker8 inv8(srcQ8_[level], srcM8_[level], xf, scoreMap, (float)target, slope);
        cv::parallel_for_(cv::Range(0, scoreMap.rows), inv8, stripes);
        return;
    }

    ScanInvoker inv(srcGx_[level], srcGy_[level], xf, scoreMap, (float)target, slope,
                    skipWeight_);
    cv::parallel_for_(cv::Range(0, scoreMap.rows), inv, stripes);
}

//***********************************************************************
//  single position scoring (sparse, used while refining)
//***********************************************************************
namespace
{
    // Gradient polarity handling (Halcon's Metric), shared by all scorers.
    // dot = model-normal . image-gradient, in [-1, 1].
    enum : int { POL_USE = 0, POL_GLOBAL = 1, POL_LOCAL = 2 };

    // P0: bilinear sample of one f16 gradient plane at a sub-pixel location.
    // The caller guarantees y,x stay inside [0, h-2] x [0, w-2]; the clamp at
    // the end covers the last row/column so a border pose never reads OOB.
    inline float sampleF16Bilinear(const cv::Mat& m, float fy, float fx)
    {
        int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        int x1 = x0 + 1, y1 = y0 + 1;
        x0 = std::min(std::max(x0, 0), m.cols - 1);
        y0 = std::min(std::max(y0, 0), m.rows - 1);
        x1 = std::min(std::max(x1, 0), m.cols - 1);
        y1 = std::min(std::max(y1, 0), m.rows - 1);
        const float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
        const float v00 = f16ToF32(m.ptr<uint16_t>(y0)[x0]);
        const float v01 = f16ToF32(m.ptr<uint16_t>(y0)[x1]);
        const float v10 = f16ToF32(m.ptr<uint16_t>(y1)[x0]);
        const float v11 = f16ToF32(m.ptr<uint16_t>(y1)[x1]);
        const float top = v00 + (v01 - v00) * tx;
        const float bot = v10 + (v11 - v10) * tx;
        return top + (bot - top) * ty;
    }


    inline float scorePos(const cv::Mat& sx, const cv::Mat& sy, const XForm& xf,
                          int ay, int ax, float limit, int metric = POL_USE,
                          float w0 = 0.f)
    {
        const int n = xf.n;
        const int16_t* pox = xf.ox.data();
        const int16_t* poy = xf.oy.data();
        const float* pnx = xf.nx.data();
        const float* pny = xf.ny.data();
        float sum = 0.f;
        int   cnt = 0;
        const int minKept = std::max(8, n / 4);
        const float lim = limit * n;
        for (int k = 0; k < n; k++)
        {
            int yy = ay + poy[k];
            int xx = ax + pox[k];
            const uint16_t* rx = sx.ptr<uint16_t>(yy);
            const uint16_t* ry = sy.ptr<uint16_t>(yy);
            float ga = f16ToF32(rx[xx]);
            float gb = f16ToF32(ry[xx]);
            if (w0 > 0.f)
            {
                // skipWeight: the sampled vector length IS the soft weight
                // (w = min(|grad|/minContrast,1)); points below w0 match
                // Halcon's "gradient magnitude below MinContrast is not
                // scored" and are excluded from the mean.
                if (ga * ga + gb * gb < w0 * w0) continue;
            }
            float d = pnx[k] * ga + pny[k] * gb;
            if (metric == POL_LOCAL) d = std::fabs(d);
            sum += d;
            cnt++;

            if (limit > -9.f && k > (n >> 2))
            {
                // Every term is at most 1 in magnitude, so either
                //   |sum| <= |partial| + (n - k - 1)   (use / global polarity)
                //   sum   <=  partial  + (n - k - 1)   (local, monotone)
                // holds.  When even that upper bound is below the target the
                // position cannot qualify and the scan stops early.
                const float reach = (metric == POL_GLOBAL ? std::fabs(sum) : sum)
                                    + (float)(n - k - 1);
                if (reach < lim)
                    return -1.f;
            }
        }
        if (metric == POL_GLOBAL) sum = std::fabs(sum);
        if (w0 > 0.f)
        {
            if (cnt < minKept) return -1.f;       // not enough usable gradient
            return sum / cnt;                     // mean over the kept points
        }
        return sum / n;
    }

    // P0: sub-pixel counterpart of scorePos().  The centre (ay, ax) is allowed
    // to be fractional and each model point uses its exact rotated offset
    // (xf.fx/fy), so the image gradients are bilinear-interpolated instead of
    // snapped to a pixel.  No early-exit pruning here: this runs only on the few
    // surviving candidates and a partial-sum bound is not valid once sampling
    // is interpolated.  Borders are handled inside sampleF16Bilinear.
    inline float scorePosSubpix(const cv::Mat& sx, const cv::Mat& sy, const XForm& xf,
                                 float ay, float ax, int metric = POL_USE,
                                 float w0 = 0.f)
    {
        const int n = xf.n;
        float sum = 0.f;
        int   cnt = 0;
        const int minKept = std::max(8, n / 4);
        for (int k = 0; k < n; k++)
        {
            const float gx = sampleF16Bilinear(sx, ay + xf.fy[k], ax + xf.fx[k]);
            const float gy = sampleF16Bilinear(sy, ay + xf.fy[k], ax + xf.fx[k]);
            if (w0 > 0.f)
            {
                const float len2 = gx * gx + gy * gy;   // = w^2
                if (len2 < w0 * w0) continue;
            }
            float d = xf.nx[k] * gx + xf.ny[k] * gy;
            if (metric == POL_LOCAL) d = std::fabs(d);
            sum += d;
            cnt++;
        }
        if (metric == POL_GLOBAL) sum = std::fabs(sum);
        if (w0 > 0.f && cnt < minKept) return -1.f;
        if (w0 > 0.f) return cnt > 0 ? sum / cnt : 0.f;
        return n > 0 ? sum / n : 0.f;
    }

    // robust re-scoring of the final matches.  search/refine use mean (scorePos);
    // this only re-aggregates the per-point dot products at the found pose so a
    // few flipped points do not drag the reported confidence down.
    //
    // The polarity transform is applied to the per-point dots *before*
    // aggregating, so every score mode composes with it:
    //   metric 1 (global): if the total is negative, all dots are negated
    //   metric 2 (local) : every dot is replaced by |dot|
    // metric 0 with mode 0 gives exactly the same number as scorePos().
    inline float scorePosMode(const cv::Mat& sx, const cv::Mat& sy, const XForm& xf,
                              int ay, int ax, int mode, int metric = POL_USE)
    {
        const int n = xf.n;
        const int16_t* pox = xf.ox.data();
        const int16_t* poy = xf.oy.data();
        const float* pnx = xf.nx.data();
        const float* pny = xf.ny.data();

        std::vector<float> dots;
        dots.resize((size_t)n);
        for (int k = 0; k < n; k++)
        {
            const uint16_t* rx = sx.ptr<uint16_t>(ay + poy[k]);
            const uint16_t* ry = sy.ptr<uint16_t>(ay + poy[k]);
            dots[(size_t)k] = pnx[k] * f16ToF32(rx[ax + pox[k]]) +
                              pny[k] * f16ToF32(ry[ax + pox[k]]);
        }

        if (metric == POL_GLOBAL)
        {
            double tot = 0;
            for (int k = 0; k < n; k++) tot += dots[(size_t)k];
            if (tot < 0)
                for (int k = 0; k < n; k++) dots[(size_t)k] = -dots[(size_t)k];
        }
        else if (metric == POL_LOCAL)
        {
            for (int k = 0; k < n; k++) dots[(size_t)k] = std::fabs(dots[(size_t)k]);
        }

        if (mode == 0)   // mean
        {
            double sum = 0;
            for (int k = 0; k < n; k++) sum += dots[(size_t)k];
            return n > 0 ? (float)(sum / n) : 0.f;
        }

        if (mode == 1)   // positive-mean: drop points whose direction flipped
        {
            double sum = 0;
            int cnt = 0;
            for (int k = 0; k < n; k++)
                if (dots[(size_t)k] > 0.f) { sum += dots[(size_t)k]; cnt++; }
            return cnt > 0 ? (float)(sum / cnt) : 0.f;
        }

        if (mode == 3)   // clipped-mean: same sum as mode 1, but n stays in the
                         // denominator, so a pose that only covers half of the
                         // model cannot reach a high score the way mode 1 can
        {
            double sum = 0;
            for (int k = 0; k < n; k++)
                if (dots[(size_t)k] > 0.f) sum += dots[(size_t)k];
            return n > 0 ? (float)(sum / n) : 0.f;
        }

        // mode 2: trimmed-mean, drop the worst 10% of points
        const int trim = n / 10;
        if (trim <= 0 || n - 2 * trim <= 0)
        {
            double sum = 0;
            for (int k = 0; k < n; k++) sum += dots[(size_t)k];
            return n > 0 ? (float)(sum / n) : 0.f;
        }
        std::sort(dots.begin(), dots.end());
        double sum = 0;
        for (int i = trim; i < n - trim; i++) sum += dots[(size_t)i];
        return (float)(sum / (n - 2 * trim));
    }

    // one pass over the model points, returning everything the final
    // verification needs:
    //   mean         - the score the search used, averaged over the *kept* points
    //                  (skipWeight) or over all n points (skipWeight = 0)
    //   clipped      - sum of the positive terms / kept count  (mode 3)
    //   coverage     - fraction of ALL model points whose direction agrees.
    //                  Deliberately relative to n, not to the kept count: a
    //                  position where the image only has gradient on part of the
    //                  model must NOT look fully covered (that was the bug that
    //                  let skipWeight inflate partial matches to 0.97).
    //   missShare    - share of the template's 4x4 cells that are effectively
    //                  empty in the image (kept share < 0.2).  Distinguishes the
    //                  two cases skipWeight has to survive:
    //                    * blur / weak lighting -> weak points are spread out,
    //                      every cell keeps a decent share (measured 0.65~0.86,
    //                      at most 1-2 cells drop) -> missShare 0.06~0.13;
    //                    * the template is only partly present (repetitive
    //                      texture / wrong instance) -> whole rows of cells go
    //                      empty (measured: half the template pasted -> 8 of 16
    //                      cells at 0.00..0.16) -> missShare 0.5.
    //                  cellMissMax rejects the second kind and keeps the first.
    struct PoseScore { float mean, clipped, coverage, missShare; };

    inline PoseScore scorePoseDetail(const cv::Mat& sx, const cv::Mat& sy, const XForm& xf,
                                     int ay, int ax, int metric = POL_USE,
                                     float w0 = 0.f)
    {
        const int n = xf.n;
        const int R = std::max(1, xf.R);
        const int GRID = 4;                 // 4x4 template cells
        const int CELLS = GRID * GRID;
        // "图像梯度是否足够强"的参考阈值：评分口径用 skipWeight，但它为 0
        // （默认 = Halcon 口径，全部模型点都参与平均）时，这个判据不能跟着失效
        // —— 否则 missShare 恒为 0，cellMissMax 等于关掉。故回落到 0.9。
        const float wCell = (w0 > 0.f) ? w0 : 0.9f;
        // 分数口径（2026-11-15）：方向余弦，即 Halcon 的"归一化梯度点积"。
        // 存储的梯度平面是 单位方向 * w，其中
        //     w = 1                 (|grad| >= minContrast)
        //     w = (|grad|/mc)^2     (|grad| <  minContrast)
        // 所以 dot / |stored| 就是方向余弦；Halcon 的分母永远是全部模型点，
        // 且"该处没有边"时点积记 0 —— 这里用 kEdgeMin 表示"没有边"。
        // 不做这一步时（旧行为）弱边点的贡献被 w 衰减到 0.1~0.6，data1 上
        // 全点均值只有 0.636，而 Halcon 是 0.956；做完这一步两者同量级。
        const float kEdgeMin = []()
        {
            const char* e = std::getenv("SM_EDGE_MIN");       // calibration knob
            return e ? (float)std::atof(e) : 0.05f;
        }();
        static const bool cosNorm = (std::getenv("SM_NO_COSNORM") == nullptr);
        int cellTot[CELLS];
        int cellKept[CELLS];
        for (int c = 0; c < CELLS; c++) { cellTot[c] = 0; cellKept[c] = 0; }

        std::vector<float> dots((size_t)n);
        std::vector<unsigned char> strong((size_t)n);
        double total = 0;
        int keptAll = 0;
        for (int k = 0; k < n; k++)
        {
            // 亚像素梯度采样（内建）：在真实旋转位置双线性插值，消除整数取整抖动
            const float gx = sampleF16Bilinear(sx, (float)ay + xf.fy[k], (float)ax + xf.fx[k]);
            const float gy = sampleF16Bilinear(sy, (float)ay + xf.fy[k], (float)ax + xf.fx[k]);
            const float len2 = gx * gx + gy * gy;         // = w^2 (soft weight)
            const bool st = (w0 <= 0.f) || (len2 >= w0 * w0);
            strong[(size_t)k] = st ? 1 : 0;
            {
                const float raw = xf.nx[k] * gx + xf.ny[k] * gy;
                float d = raw;
                if (cosNorm)
                {
                    const float len = std::sqrt(len2);
                    d = (len >= kEdgeMin) ? raw / len : 0.f;   // 没有边 -> 0
                    d = std::min(1.f, std::max(-1.f, d));
                }
                dots[(size_t)k] = d;
                total += d;
            }
            if (st) keptAll++;

            {
                const int cx = std::min(GRID - 1, std::max(0, (int)((xf.ox[k] + R) * GRID / (2 * R + 1))));
                const int cy = std::min(GRID - 1, std::max(0, (int)((xf.oy[k] + R) * GRID / (2 * R + 1))));
                const int c = cy * GRID + cx;
                cellTot[c]++;
                if (len2 >= wCell * wCell) cellKept[c]++;
            }
        }

        const bool flip = (metric == POL_GLOBAL && total < 0.0);

        // 默认口径（skipWeight = 0）：mean/clipped 对全部 n 个模型点取平均 = Halcon；
        // 设了 skipWeight 则只对"有效点"取平均，coverage 仍以全部 n 为分母。
        const int minKept = std::max(8, n / 4);
        double sum = 0.0, posSum = 0.0, absSum = 0.0;
        int cnt = 0;
        for (int k = 0; k < n; k++)
        {
            if (w0 > 0.f && !strong[(size_t)k]) continue;
            float d = dots[(size_t)k];
            if (flip) d = -d;
            absSum += std::fabs(d);
            sum += d;
            if (d > 0.f) { posSum += d; cnt++; }
        }

        PoseScore ps;
        const int nk = (w0 > 0.f) ? keptAll : n;
        const float invK = nk > 0 ? 1.f / (float)nk : 0.f;
        const float invN = n > 0 ? 1.f / (float)n : 0.f;

        if (metric == POL_LOCAL)
        {
            ps.mean = (float)(absSum * invK);
            ps.clipped = ps.mean;
        }
        else
        {
            ps.mean = (float)(sum * invK);
            ps.clipped = (float)(posSum * invK);
        }

        // coverage: agreeing points over ALL model points
        {
            int agree = 0;
            for (int k = 0; k < n; k++)
            {
                float d = dots[(size_t)k];
                if (flip) d = -d;
                if (d > 0.f) agree++;
            }
            ps.coverage = n > 0 ? (float)agree * invN : 0.f;
        }

        // A cell counts as "missing" when less than kCellKeepMin of its model
        // points sit on a usable image gradient.
        //
        // Calibration (2026-11-15, SM_DUMP_CELLS + _ai_tmp/cells_probe.ps1):
        //   - the U-bend / serpentine template of this log, pose shifted by 86 px
        //     (the case users report as "overlapping duplicates with high score")
        //     keeps 0.18~0.37 in the shifted-away half -> missShare 0.10 at 0.2
        //     but 0.375 at 0.4 (rejected) and 0.5 at 0.5.
        //   - data2 (-90deg cluster, weak appearance) keeps 0.00~0.5 scattered,
        //     so missShare becomes 0.4~0.67 at 0.4 and 0.5~0.71 at 0.5
        //     -> tightening globally costs 8 of 35 true detections.
        // i.e. "part of the template has no image gradient" cannot separate a
        // shifted false pose from a weak but correct one, so 0.2 stays the
        // default; SM_CELL_KEEP lets an application tighten it when its targets
        // really do have dense edges everywhere.
        const float kCellKeepMin = []()
        {
            const char* e = std::getenv("SM_CELL_KEEP");      // calibration knob
            return e ? (float)std::atof(e) : 0.2f;
        }();
        {
            int judged = 0, missing = 0;
            for (int c = 0; c < CELLS; c++)
            {
                if (cellTot[c] < 4) continue;              // too few points to judge
                judged++;
                const float share = (float)cellKept[c] / (float)cellTot[c];
                if (share < kCellKeepMin) missing++;
            }
            ps.missShare = judged > 0 ? (float)missing / (float)judged : 0.f;
        }
        // diagnostics: SM_DUMP_CELLS=1 prints the 4x4 table for every scored pose,
        // which is what the cellMissMax default has to be calibrated against.
        if (std::getenv("SM_DUMP_CELLS") != nullptr)
        {
            std::cout << "  [cells] score=" << ps.mean << " cover=" << ps.coverage
                      << " missShare=" << ps.missShare << "\n";
            for (int cy = 0; cy < GRID; cy++)
            {
                std::cout << "  [cells]  ";
                for (int cx = 0; cx < GRID; cx++)
                {
                    const int c = cy * GRID + cx;
                    if (cellTot[c] == 0) { printf("   -   "); continue; }
                    printf(" %5.2f ", (float)cellKept[c] / (float)cellTot[c]);
                }
                printf("\n");
            }
        }

        if (w0 > 0.f && nk < minKept)
        {
            // not enough usable gradient anywhere -> the position is not believable
            ps.mean = -1.f;
            ps.clipped = -1.f;
        }
        return ps;
    }
}

//=======================================================================
//  refine all candidates one level down
//=======================================================================
//=======================================================================
//  level-0 geometry diagnostics
//
//  Two numbers explain the failure mode where a degraded model (few points,
//  or points that ended up on one line) reports a very high score at a wrong
//  position: the normalised dot product is a mean over the points, so with few
//  points it is arithmetic over little evidence, and points lying along one
//  line only constrain the direction across that line - the model can slide
//  along it (aperture problem).  Both grow when the contrast threshold is
//  pushed until almost all points disappear.
//=======================================================================
void ShapeMatcher::computeGeometryFacts(const LevelData& ld) const
{
    const int n = (int)ld.points.size();
    spreadRatio_ = 0.f;
    normalPeakShare_ = 0.f;
    if (n <= 1) return;

    // ---- gradient direction concentration (18 bins of 10 degrees) --------
    int bins[18] = { 0 };
    double cx = 0, cy = 0;
    for (int k = 0; k < n; k++)
    {
        const ModelPoint& p = ld.points[(size_t)k];
        int b = (int)std::floor((std::atan2(p.gy, p.gx) * 180.0 / CV_PI + 180.0) / 10.0);
        if (b < 0) b = 0;
        if (b > 17) b = 17;
        bins[b]++;
        cx += p.x;
        cy += p.y;
    }
    int peak = 0;
    for (int i = 0; i < 18; i++) peak = std::max(peak, bins[i]);
    normalPeakShare_ = (float)peak / (float)n;

    // ---- spread of the point positions along the principal axes ----------
    cx /= n;
    cy /= n;
    double sxx = 0, syy = 0, sxy = 0;
    for (int k = 0; k < n; k++)
    {
        const double dx = ld.points[(size_t)k].x - cx;
        const double dy = ld.points[(size_t)k].y - cy;
        sxx += dx * dx;
        syy += dy * dy;
        sxy += dx * dy;
    }
    sxx /= n; syy /= n; sxy /= n;
    const double tr = sxx + syy;
    const double det = sxx * syy - sxy * sxy;
    const double disc = std::sqrt(std::max(0.0, tr * tr / 4.0 - det));
    const double l1 = tr / 2.0 + disc;
    const double l2 = std::max(1e-9, tr / 2.0 - disc);
    spreadRatio_ = (float)std::sqrt(l1 / l2);
}

void ShapeMatcher::refineLevel(int level, std::vector<Candidate>& cands,
                               int maxKeep, double minScore, int metric,
                               int64_t deadline, bool* timedOut)
{
    (void)deadline; (void)timedOut;   // 逐候选超时检查见下方 parallel 循环
    const int stride = 1 << level;
    const int nA = (int)angleGrid_.size();
    const int nS = (int)scaleGrid_.size();
    // the parent position carries an error of up to one parent pixel, which is
    // two pixels of this level -> the window has to cover at least +/-2
    const int win = 2;
    const int W = 2 * win + 1;

    // the (angle, scale) window every candidate searches, resolved up front so
    // that the workers only ever read the XForm cache.  warmXForm inserts, and
    // an insert from a worker thread would race.
    struct Window { int as[5], ss[5]; int na, ns; };
    const int nc = (int)cands.size();
    std::vector<Window> wins(nc);
    for (int ci = 0; ci < nc; ci++)
    {
        const Candidate& sc = cands[ci];
        Window& w = wins[ci];
        w.na = w.ns = 0;
        for (int k = -2; k <= 2; k++)
        {
            int a = sc.angleIdx + k * stride;
            if (a >= 0 && a < nA) w.as[w.na++] = a;
            int s = sc.scaleIdx + k * stride;
            if (s >= 0 && s < nS) w.ss[w.ns++] = s;
        }
        if (w.na == 0) w.as[w.na++] = sc.angleIdx;
        if (w.ns == 0) w.ss[w.ns++] = sc.scaleIdx;
        for (int i = 0; i < w.na; i++)
            for (int j = 0; j < w.ns; j++)
                warmXForm(level, w.as[i], w.ss[j]);
    }

    // the candidates are independent, so the refinement runs in parallel.  Each
    // result goes to its own slot and is merged in candidate order afterwards,
    // which means the sequence handed to the sort below is exactly the one the
    // serial version produced.
    std::vector<Candidate> refined(nc);
    std::vector<unsigned char> found(nc, 0);
    cv::parallel_for_(cv::Range(0, nc), [&](const cv::Range& r)
    {
        for (int ci = r.start; ci < r.end; ci++)
        {
            const Window& w = wins[ci];
            const int na = w.na, ns = w.ns;
            const Candidate sc = cands[ci];

            // 超时：本候选直接跳过（found[ci] 保持 0，会被下游丢弃）
            if (deadline && cv::getTickCount() > deadline)
            {
                if (timedOut) *timedOut = true;
                continue;
            }

            Candidate best;
            best.score = -2.f;
            best.level = level;
            std::vector<float> scoreBuf((size_t)na * ns * W * W, -2.f);
            int bi = 0, bj = 0, bdx = win, bdy = win;

            for (int i = 0; i < na; i++)
            {
                for (int j = 0; j < ns; j++)
                {
                    // read only lookup: a worker thread must never insert into
                    // the XForm cache.  The warm up above already put every pose
                    // of this level into it, so the fallback is dead code - it
                    // only exists so a miss cannot corrupt the cache.
                    const XForm* pxf = findXForm(level, w.as[i], w.ss[j]);
                    XForm localXf;
                    if (!pxf)
                    {
                        buildXForm(level, w.as[i], w.ss[j], localXf);
                        pxf = &localXf;
                    }
                    const XForm& xf = *pxf;
                    float limit = (i == 0 && j == 0) ? -10.f : best.score;
                    for (int dy = -win; dy <= win; dy++)
                    {
                        for (int dx = -win; dx <= win; dx++)
                        {
                            int bx = cvRound(sc.pos.x * 2.f) + dx;
                            int by = cvRound(sc.pos.y * 2.f) + dy;
                            bx = std::min(std::max(bx, xf.R), srcGx_[level].cols - 1 - xf.R);
                            by = std::min(std::max(by, xf.R), srcGx_[level].rows - 1 - xf.R);
                            float v = (bx < xf.R || by < xf.R)
                                          ? -2.f
                                          : scorePos(srcGx_[level], srcGy_[level], xf, by, bx, limit, metric,
                                                     skipWeight_);
                            scoreBuf[(((size_t)i * ns + j) * W + (dy + win)) * W + (dx + win)] = v;
                            if (v > best.score)
                            {
                                best.score = v;
                                best.level = level;
                                best.pos = cv::Point2f((float)bx, (float)by);
                                best.angleIdx = w.as[i];
                                best.scaleIdx = w.ss[j];
                                best.angle = (float)angleGrid_[w.as[i]];
                                best.scale = (float)scaleGrid_[w.ss[j]];
                                bi = i; bj = j; bdx = dx + win; bdy = dy + win;
                                limit = v;
                            }
                        }
                    }
                }
            }

            // sub pixel position of the winning pose (parabola through its neighbours)
            if (best.score > -1.f)
            {
                const size_t base = ((size_t)bi * ns + bj) * W * W;
                float dx = 0, dy = 0;
                if (bdx > 0 && bdx + 1 < W)
                {
                    float a = scoreBuf[base + bdy * W + bdx - 1];
                    float b = scoreBuf[base + bdy * W + bdx + 1];
                    float den = 2.f * best.score - a - b;
                    if (den > 1e-6f) dx = std::min(1.f, std::max(-1.f, 0.5f * (b - a) / den));
                }
                if (bdy > 0 && bdy + 1 < W)
                {
                    float a = scoreBuf[base + (bdy - 1) * W + bdx];
                    float b = scoreBuf[base + (bdy + 1) * W + bdx];
                    float den = 2.f * best.score - a - b;
                    if (den > 1e-6f) dy = std::min(1.f, std::max(-1.f, 0.5f * (b - a) / den));
                }
                best.pos.x += dx;
                best.pos.y += dy;
                refined[ci] = best;
                found[ci] = 1;
            }
        }
    }, std::max(1.0, (double)nc / 2.0));

    std::vector<Candidate> merged;
    merged.reserve(nc);
    for (int ci = 0; ci < nc; ci++)
        if (found[ci]) merged.push_back(refined[ci]);
    refined.swap(merged);

    // keep the best ones only
    std::sort(refined.begin(), refined.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    cands.clear();
    const int rs = std::min((int)refined.size(), std::max(maxKeep, 1));
    for (size_t i = 0; i < refined.size() && (int)cands.size() < rs; i++)
    {
        const Candidate& c = refined[i];
        bool dup = false;
        for (size_t j = 0; j < cands.size(); j++)
        {
            const Candidate& o = cands[j];
            if (std::abs(c.pos.x - o.pos.x) <= 3.f && std::abs(c.pos.y - o.pos.y) <= 3.f &&
                angleDiffWrapped(c.angle, o.angle) <= 1e-4f + (float)levelAngleStep_[level] &&
                std::abs(c.scale - o.scale) <= 1e-4f + (float)levelScaleStep_[level] * 0.5f)
            {
                dup = true; break;
            }
        }
        if (!dup) cands.push_back(c);
    }
    (void)minScore;
}

//=======================================================================
//  sub-pixel refinement (parabolic fit in x, y, angle and scale)
//=======================================================================
void ShapeMatcher::subpixelRefine(int level, Candidate& c, int mode, int metric)
{
    XForm& xf0 = getXForm(level, c.angleIdx, c.scaleIdx);
    // scorePos is sampled at ax+-1 and ay+-1 and the model reaches +-R around
    // each of those, so the centre has to stay one pixel clear of the border.
    // refineLevel only clamps to [R, w-1-R], which leaves ax+1+R one past the
    // last column - without this clamp that read goes out of bounds.
    const int R = xf0.R;
    const int w = srcGx_[level].cols, h = srcGx_[level].rows;
    const int lo = R + 1, hiX = w - 2 - R, hiY = h - 2 - R;
    if (lo > hiX || lo > hiY) return;          // image too small, keep the pose
    int ax = std::min(std::max(cvRound(c.pos.x), lo), hiX);
    int ay = std::min(std::max(cvRound(c.pos.y), lo), hiY);

    // 亚像素梯度采样（内建）：在模型点真实旋转位置双线性插值采样图像梯度，
    // 消除整数取整的 <=0.5px 抖动。+-1 的抛物线探测仍整像素移动，保证有限差分
    // 曲率可比。粗扫仍走整数卷积核，这里只作用于少量存活候选。
    auto eval = [&](const XForm& xf, int cy, int cx) -> float
    {
        return scorePosSubpix(srcGx_[level], srcGy_[level], xf,
                              (float)cy, (float)cx, metric, skipWeight_);
    };

    float s0 = eval(xf0, ay, ax);
    float spx = eval(xf0, ay, ax + 1);
    float smx = eval(xf0, ay, ax - 1);
    float spy = eval(xf0, ay + 1, ax);
    float smy = eval(xf0, ay - 1, ax);

    float dx = 0.f, dy = 0.f;
    float den = 2.f * s0 - spx - smx;
    if (den > 1e-6f) dx = 0.5f * (spx - smx) / den;
    den = 2.f * s0 - spy - smy;
    if (den > 1e-6f) dy = 0.5f * (spy - smy) / den;
    dx = std::min(std::max(dx, -1.f), 1.f);
    dy = std::min(std::max(dy, -1.f), 1.f);
    c.pos.x = ax + dx;
    c.pos.y = ay + dy;
    c.score = s0;

    const int nA = (int)angleGrid_.size();
    const int nS = (int)scaleGrid_.size();
    // angle
    if (nA > 2)
    {
        int ap = std::min(c.angleIdx + 1, nA - 1), am = std::max(c.angleIdx - 1, 0);
        float v0 = s0;
        // every getXForm() may purge the cache, so no reference may be kept
        // across two calls
        float vp = eval(getXForm(level, ap, c.scaleIdx), ay, ax);
        float vm = eval(getXForm(level, am, c.scaleIdx), ay, ax);
        float d = 2.f * v0 - vp - vm;
        if (d > 1e-6f)
        {
            float t = 0.5f * (vp - vm) / d;
            t = std::min(std::max(t, -1.f), 1.f);
            c.angle = (float)(angleGrid_[c.angleIdx] + t * angleStep_);
        }
    }
    // scale
    if (nS > 2)
    {
        int sp = std::min(c.scaleIdx + 1, nS - 1), sm = std::max(c.scaleIdx - 1, 0);
        float v0 = s0;
        float vp = eval(getXForm(level, c.angleIdx, sp), ay, ax);
        float vm = eval(getXForm(level, c.angleIdx, sm), ay, ax);
        float d = 2.f * v0 - vp - vm;
        if (d > 1e-6f)
        {
            float t = 0.5f * (vp - vm) / d;
            t = std::min(std::max(t, -1.f), 1.f);
            c.scale = (float)(scaleGrid_[c.scaleIdx] + t * scaleStep_);
        }
    }

    // 2 = least_squares: the parabola only looks at +-1 samples in each
    // direction, so on soft edges it stops where the score surface is still
    // curved.  Gauss-Newton on the actual residuals polishes the pose, starting
    // from the interpolated one.
    if (mode >= 2)
        leastSquaresRefine(level, c, metric);
}

//=======================================================================
//  polarity aware scan (Halcon's Metric)
//=======================================================================
void ShapeMatcher::scanPose(int level, const XForm& xf, cv::Mat& scoreMap, int& R,
                            double target, double greed, int metric, bool nested)
{
    if (metric == POL_USE)
    {
        correlateSparseX(level, xf, scoreMap, R, target, greed, nested);
        return;
    }

    if (metric == POL_GLOBAL)
    {
        // |mean(dot)| == max(mean(dot), mean(-dot)).  Running the existing scan
        // a second time with negated normals therefore gives exactly the
        // globally inverted score, and the greedy pruning stays valid for both
        // passes (it bounds `sum`, and one of the two carries the positive sign).
        correlateSparseX(level, xf, scoreMap, R, target, greed, nested);
        XForm nxf = xf;                       // copies the offsets too
        for (size_t i = 0; i < nxf.nx.size(); i++) nxf.nx[i] = -nxf.nx[i];
        for (size_t i = 0; i < nxf.ny.size(); i++) nxf.ny[i] = -nxf.ny[i];
        // the integer packets must follow the negated normals, otherwise the
        // 8-bit kernel would score with the original direction
        quantizeNormals(nxf);
        cv::Mat negMap;
        int R2 = R;
        correlateSparseX(level, nxf, negMap, R2, target, greed, nested);
        cv::max(scoreMap, negMap, scoreMap);
        return;
    }

    scanLocalPolarity(level, xf, scoreMap, R, target, greed, nested);
}

void ShapeMatcher::scanLocalPolarity(int level, const XForm& xf, cv::Mat& scoreMap, int& R,
                                     double target, double greed, bool nested)
{
    R = xf.R;
    const int K = 2 * R + 1;
    const cv::Mat& gx = srcGx_[level];
    const cv::Mat& gy = srcGy_[level];
    CV_Assert(gx.type() == CV_16F && gy.type() == CV_16F);
    if (K > gx.rows || K > gx.cols)
    {
        // model bigger than the level image: no valid position (see correlateSparseX)
        scoreMap.release();
        return;
    }
    scoreMap.create(gx.rows - K + 1, gx.cols - K + 1, CV_32F);

    const int n = xf.n;
    const int W = scoreMap.cols;
    if (n <= 0 || W <= 0 || scoreMap.rows <= 0) return;

    const int16_t* pox = xf.ox.data();
    const int16_t* poy = xf.oy.data();
    const float* pnx = xf.nx.data();
    const float* pny = xf.ny.data();
    const float invN = 1.f / (float)n;

    // |dot| is in [0, 1] and the sum only grows, so the same monotone bound as
    // the normal scan applies: once sum + remaining cannot reach the target the
    // position is dead.
    const double g = std::min(std::max(greed, 0.0), 0.98);
    const float limN = (float)(target * n);
    const float slope = (float)((1.0 - g * target) / ((1.0 - g) * std::max(1, n)));

    auto scanRows = [&](int y0, int y1)
    {
        const int minKept = std::max(8, n / 4);
        for (int y = y0; y < y1; y++)
        {
            float* orow = scoreMap.ptr<float>(y);
            const int yBase = y + R;
            for (int x = 0; x < W; x++)
            {
                const int xBase = x + R;
                float sum = 0.f;
                int   cnt = 0;
                bool dead = false;
                for (int k = 0; k < n; k++)
                {
                    const uint16_t* rx = gx.ptr<uint16_t>(yBase + poy[k]);
                    const uint16_t* ry = gy.ptr<uint16_t>(yBase + poy[k]);
                    const int ix = xBase + pox[k];
                    const float ga = f16ToF32(rx[ix]);
                    const float gb = f16ToF32(ry[ix]);
                    if (skipWeight_ > 0.f &&
                        ga * ga + gb * gb < skipWeight_ * skipWeight_)
                        continue;                       // weak gradient: skip
                    const float d = pnx[k] * ga + pny[k] * gb;
                    sum += std::fabs(d);
                    cnt++;
                    const int m = k + 1;
                    if (((m & 7) == 0) || m == n)
                    {
                        const float limA = ((target - 1.f) + slope * m) * m;
                        const float limB = limN * (float)m * invN;
                        const float lim = std::max(0.f, std::min(limA, limB));
                        if (sum + (float)(n - m) < lim) { dead = true; break; }
                    }
                }
                orow[x] = (dead || cnt < minKept) ? -1.f
                                                  : (cnt > 0 ? sum / cnt : -1.f);
            }
        }
    };

    if (nested)
        scanRows(0, scoreMap.rows);
    else
        cv::parallel_for_(cv::Range(0, scoreMap.rows),
                          [&](const cv::Range& r) { scanRows(r.start, r.end); },
                          std::max(1.0, (double)scoreMap.rows / 4.0));
}

//=======================================================================
//  least squares sub-pixel refinement helpers
//=======================================================================
namespace
{
    struct SubXForm
    {
        std::vector<float> fx, fy;   // centre-relative position, sub-pixel
        std::vector<float> nx, ny;   // unit normal, rotated
        int n = 0;
    };

    inline void buildSubXForm(const std::vector<ModelPoint>& pts, double angle,
                              double scale, SubXForm& s)
    {
        const int n = (int)pts.size();
        s.n = n;
        s.fx.resize((size_t)n); s.fy.resize((size_t)n);
        s.nx.resize((size_t)n); s.ny.resize((size_t)n);
        const float ca = (float)std::cos(angle), sa = (float)std::sin(angle);
        const float sc = (float)scale;
        for (int k = 0; k < n; k++)
        {
            const ModelPoint& mp = pts[(size_t)k];
            s.fx[(size_t)k] = sc * (mp.x * ca + mp.y * sa);
            s.fy[(size_t)k] = sc * (-mp.x * sa + mp.y * ca);
            const float inv = (mp.mag > 1e-6f) ? (1.f / mp.mag) : 0.f;
            s.nx[(size_t)k] = (mp.gx * ca + mp.gy * sa) * inv;
            s.ny[(size_t)k] = (-mp.gx * sa + mp.gy * ca) * inv;
        }
    }

    inline float sampleBilinear(const cv::Mat& m, int x0, int y0, float tx, float ty)
    {
        const uint16_t* r0 = m.ptr<uint16_t>(y0);
        const uint16_t* r1 = m.ptr<uint16_t>(y0 + 1);
        const float a = f16ToF32(r0[x0]), b = f16ToF32(r0[x0 + 1]);
        const float c = f16ToF32(r1[x0]), d = f16ToF32(r1[x0 + 1]);
        return (a * (1.f - tx) + b * tx) * (1.f - ty) + (c * (1.f - tx) + d * tx) * ty;
    }

    // Per-point dot products at a sub-pixel pose.  Returns false when the model
    // would reach outside the image, in which case the caller rejects the step.
    inline bool dotsAt(const cv::Mat& gx, const cv::Mat& gy, const SubXForm& s,
                       float px, float py, int metric, std::vector<float>& dots)
    {
        const int n = s.n;
        const int w = gx.cols, h = gx.rows;
        dots.resize((size_t)n);
        for (int k = 0; k < n; k++)
        {
            const float fx = px + s.fx[(size_t)k];
            const float fy = py + s.fy[(size_t)k];
            const int x0 = (int)std::floor(fx);
            const int y0 = (int)std::floor(fy);
            if (x0 < 0 || y0 < 0 || x0 + 1 >= w || y0 + 1 >= h)
                return false;
            const float tx = fx - (float)x0, ty = fy - (float)y0;
            const float ix = sampleBilinear(gx, x0, y0, tx, ty);
            const float iy = sampleBilinear(gy, x0, y0, tx, ty);
            const float d = s.nx[(size_t)k] * ix + s.ny[(size_t)k] * iy;
            dots[(size_t)k] = (metric == POL_LOCAL) ? std::fabs(d) : d;
        }
        return true;
    }

    // solve a small system A z = b (n <= 6) with partial pivoting
    inline bool solveSmall(double A[6][6], double b[6], int n, double z[6])
    {
        for (int c = 0; c < n; c++)
        {
            int piv = c;
            for (int r = c + 1; r < n; r++)
                if (std::fabs(A[r][c]) > std::fabs(A[piv][c])) piv = r;
            if (std::fabs(A[piv][c]) < 1e-14) return false;
            if (piv != c)
            {
                for (int j = 0; j < n; j++) std::swap(A[c][j], A[piv][j]);
                std::swap(b[c], b[piv]);
            }
            for (int r = c + 1; r < n; r++)
            {
                const double f = A[r][c] / A[c][c];
                if (f == 0.0) continue;
                for (int j = c; j < n; j++) A[r][j] -= f * A[c][j];
                b[r] -= f * b[c];
            }
        }
        for (int c = n - 1; c >= 0; c--)
        {
            double s = b[c];
            for (int j = c + 1; j < n; j++) s -= A[c][j] * z[j];
            z[c] = s / A[c][c];
        }
        return true;
    }
}

//=======================================================================
//  least squares sub-pixel refinement (Halcon SubPixel = 'least_squares')
//
//  Samples the score on the 3x3 grid around the integer peak, fits
//      s(x, y) = a + b*x + c*y + d*x^2 + e*x*y + f*y^2
//  by least squares (all nine samples, so the x*y term is modelled) and takes
//  the stationary point of that surface.
//
//  Why not a Gauss-Newton on the per-point residuals: the dot products the
//  scorer uses are weighted by the image gradient magnitude, which is not 1, so
//  sum(1 - dot)^2 is not an "edge agreement" cost - it degenerates into
//  "maximise gradient magnitude" and pulls the centre onto the strongest edge
//  nearby (measured: 0.2-0.4 px bias away from the truth).  The quadratic fit
//  works on the score the search actually optimises, and the cross term makes it
//  more accurate than the separable parabola of mode 1 when the peak is skewed.
//
//  Angle and scale keep the interpolated values from mode 1: they are far less
//  well conditioned here and letting them move drags the centre off.
//=======================================================================
void ShapeMatcher::leastSquaresRefine(int level, Candidate& c, int metric)
{
    const int R = modelLevels_[level].activePoints > 0 ? getXForm(level, c.angleIdx, c.scaleIdx).R : 0;
    const cv::Mat& gx = srcGx_[level];
    const cv::Mat& gy = srcGy_[level];
    const int lo = R, hiX = gx.cols - 1 - R, hiY = gy.rows - 1 - R;
    if (lo > hiX || lo > hiY) return;

    const int ax = std::min(std::max(cvRound(c.pos.x), lo), hiX);
    const int ay = std::min(std::max(cvRound(c.pos.y), lo), hiY);

    const XForm* pxf = findXForm(level, c.angleIdx, c.scaleIdx);
    if (!pxf) return;

    // ---- 3x3 score samples ------------------------------------------------
    double s[9];
    double sx[9], sy[9];
    int ns = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
        {
            const int px = std::min(std::max(ax + dx, lo), hiX);
            const int py = std::min(std::max(ay + dy, lo), hiY);
            s[ns] = scorePos(gx, gy, *pxf, py, px, -10.f, metric, skipWeight_);
            sx[ns] = (double)(px - ax);
            sy[ns] = (double)(py - ay);
            ns++;
        }

    // ---- least squares fit of the 6 coefficients --------------------------
    double A[6][6] = { { 0 } };
    double rhs[6] = { 0 };
    for (int i = 0; i < ns; i++)
    {
        const double x = sx[i], y = sy[i];
        const double phi[6] = { 1.0, x, y, x * x, x * y, y * y };
        for (int p = 0; p < 6; p++)
        {
            rhs[p] += phi[p] * s[i];
            for (int q = 0; q < 6; q++) A[p][q] += phi[p] * phi[q];
        }
    }
    double coef[6];
    if (!solveSmall(A, rhs, 6, coef)) return;

    const double b = coef[1], cc = coef[2], d = coef[3], e = coef[4], f = coef[5];
    const double D = 4.0 * d * f - e * e;
    if (D <= 1e-12 || d >= 0.0) return;          // not a maximum: keep the pose

    double dx = (-2.0 * f * b + e * cc) / D;
    double dy = (e * b - 2.0 * d * cc) / D;
    dx = std::min(std::max(dx, -1.0), 1.0);
    dy = std::min(std::max(dy, -1.0), 1.0);

    // guard: the fitted surface must not be wildly better than the samples
    const double atPeak = scorePos(gx, gy, *pxf,
                                   std::min(std::max(cvRound(ay + dy), lo), hiY),
                                   std::min(std::max(cvRound(ax + dx), lo), hiX),
                                   -10.f, metric, skipWeight_);
    if (atPeak < -0.5f) return;

    c.pos = cv::Point2f((float)(ax + dx), (float)(ay + dy));
}

#if 0   // superseded by the quadratic-fit least squares above
//=======================================================================
//  local maxima of a score map
//=======================================================================
void ShapeMatcher::leastSquaresRefine(int level, Candidate& c, int metric)
{
    const std::vector<ModelPoint>& pts = modelLevels_[level].points;
    const int n = (int)pts.size();
    if (n < 8) return;

    const cv::Mat& gx = srcGx_[level];
    const cv::Mat& gy = srcGy_[level];

    double th[4] = { (double)c.pos.x, (double)c.pos.y,
                     (double)c.angle, (double)c.scale };
    const double th0[4] = { th[0], th[1], th[2], th[3] };

    const double hStep[4] = {
        0.08, 0.08,
        std::max(1e-5, std::fabs(angleStep_) * 0.20),
        std::max(1e-6, std::fabs(scaleStep_) * 0.20)
    };
    // how far it may walk from the grid pose, so a bad start cannot slide onto
    // a different object
    const double maxDxy   = 2.0;
    const double maxDang  = std::max(1e-4, std::fabs(angleStep_));
    const double maxDscal = std::max(1e-5, std::fabs(scaleStep_));

    SubXForm sxf, sPlus, sMinus;
    std::vector<float> d0, dPlus, dMinus;
    std::vector<float> wgt;
    std::vector<float> jac[4];
    wgt.resize((size_t)n);
    for (int p = 0; p < 4; p++) jac[p].resize((size_t)n);

    buildSubXForm(pts, th[2], th[3], sxf);
    if (!dotsAt(gx, gy, sxf, (float)th[0], (float)th[1], metric, d0)) return;

    double bestE = 0;
    for (int k = 0; k < n; k++)
    {
        const double r = 1.0 - (double)d0[(size_t)k];
        bestE += r * r;
    }

    // Only x and y are refined.  Angle and scale are kept at the interpolated
    // values: on these images they are far less well conditioned than the
    // position and letting them move drags the centre off by up to 0.3 px.
    const int NP = 2;
    for (int iter = 0; iter < 8; iter++)
    {
        // ---- finite-difference Jacobian of the dot products -------------
        for (int p = 0; p < NP; p++)
        {
            double tp[4], tm[4];
            for (int j = 0; j < 4; j++) { tp[j] = th[j]; tm[j] = th[j]; }
            tp[p] += hStep[p];
            tm[p] -= hStep[p];

            sPlus = sxf;            // only the centre moves
            sMinus = sxf;
            if (!dotsAt(gx, gy, sPlus, (float)tp[0], (float)tp[1], metric, dPlus)) return;
            if (!dotsAt(gx, gy, sMinus, (float)tm[0], (float)tm[1], metric, dMinus)) return;

            const float inv2h = (float)(1.0 / (2.0 * hStep[p]));
            for (int k = 0; k < n; k++)
                jac[p][(size_t)k] = (dPlus[(size_t)k] - dMinus[(size_t)k]) * inv2h;
        }
        // Points that contradict the model (d <= 0) are treated as outliers with
        // zero weight.  Without this the objective sum(1-d)^2 also rewards poses
        // where some points sit on an inverted edge (|d| -> 1), so the refinement
        // would happily slide onto the wrong polarity.
        for (int k = 0; k < n; k++) wgt[(size_t)k] = (d0[(size_t)k] > 0.f) ? 1.f : 0.f;
        double wsum = 0;
        for (int k = 0; k < n; k++) wsum += wgt[(size_t)k];
        if (wsum < 0.25 * (double)n) break;      // too few agreeing points

        // the baseline cost has to use the same weights as the step, otherwise
        // the first iteration compares two different objectives
        if (iter == 0)
        {
            bestE = 0;
            for (int k = 0; k < n; k++)
            {
                if (wgt[(size_t)k] == 0.f) continue;
                const double r = 1.0 - (double)d0[(size_t)k];
                bestE += r * r;
            }
        }

        // ---- normal equations H d = g -----------------------------------
        double H[4][4] = { { 0 } };
        double g[4] = { 0 };
        for (int p = 0; p < NP; p++)
        {
            double gp = 0;
            for (int k = 0; k < n; k++)
                gp += wgt[(size_t)k] * (double)jac[p][(size_t)k] *
                      (1.0 - (double)d0[(size_t)k]);
            g[p] = gp;
            for (int q = 0; q <= p; q++)
            {
                double s = 0;
                for (int k = 0; k < n; k++)
                    s += wgt[(size_t)k] * (double)jac[p][(size_t)k] *
                         (double)jac[q][(size_t)k];
                H[p][q] = H[q][p] = s;
            }
        }

        // Jacobi preconditioning keeps the solve well behaved when the two
        // directions have very different curvature
        double sc[4];
        for (int p = 0; p < NP; p++)
            sc[p] = std::sqrt(std::max(H[p][p], 1e-16));

        double A[4][4], b[4], z[4];
        for (int p = 0; p < NP; p++)
        {
            b[p] = g[p] / sc[p];
            for (int q = 0; q < NP; q++) A[p][q] = H[p][q] / (sc[p] * sc[q]);
        }
        for (int p = 0; p < NP; p++) A[p][p] += 1e-3;    // light damping
        if (!solveSmall(A, b, NP, z)) break;

        double dth[4] = { 0, 0, 0, 0 };
        for (int p = 0; p < NP; p++) dth[p] = z[p] / sc[p];

        dth[0] = std::min(std::max(dth[0], -0.5), 0.5);
        dth[1] = std::min(std::max(dth[1], -0.5), 0.5);

        double cand[4];
        for (int p = 0; p < 4; p++) cand[p] = th[p] + dth[p];
        cand[0] = std::min(std::max(cand[0], th0[0] - maxDxy), th0[0] + maxDxy);
        cand[1] = std::min(std::max(cand[1], th0[1] - maxDxy), th0[1] + maxDxy);

        if (!dotsAt(gx, gy, sxf, (float)cand[0], (float)cand[1], metric, dPlus)) break;

        double newE = 0;
        for (int k = 0; k < n; k++)
        {
            if (wgt[(size_t)k] == 0.f) continue;
            const double r = 1.0 - (double)dPlus[(size_t)k];
            newE += r * r;
        }
        if (newE >= bestE - 1e-12) break;    // no improvement: converged

        for (int p = 0; p < 4; p++) th[p] = cand[p];
        d0.swap(dPlus);
        bestE = newE;

        if (std::fabs(dth[0]) < 1e-3 && std::fabs(dth[1]) < 1e-3 &&
            std::fabs(dth[2]) < 1e-5 && std::fabs(dth[3]) < 1e-6) break;
    }

    c.pos   = cv::Point2f((float)th[0], (float)th[1]);
    c.angle = (float)th[2];
    c.scale = (float)th[3];
    float s = 0.f;
    for (int k = 0; k < n; k++) s += d0[(size_t)k];
    if (metric == POL_GLOBAL) s = std::fabs(s);
    c.score = s / (float)n;
}
#endif  // dead Gauss-Newton implementation

//=======================================================================
//  local maxima of a score map
//=======================================================================
namespace
{
    // serial min/max, same reason as above: cv::minMaxLoc would nest
    void minMaxSerial(const cv::Mat& m, double& mn, double& mx)
    {
        if (m.empty()) { mn = mx = 0.0; return; }
        mn = std::numeric_limits<double>::max();
        mx = -std::numeric_limits<double>::max();
        for (int y = 0; y < m.rows; y++)
        {
            const float* p = m.ptr<float>(y);
            for (int x = 0; x < m.cols; x++)
            {
                if (p[x] < mn) mn = p[x];
                if (p[x] > mx) mx = p[x];
            }
        }
    }
}

void ShapeMatcher::extractCandidates(const cv::Mat& scoreMap, int level, int aIdx, int sIdx,
                                     int R, double thresh, std::vector<Candidate>& out,
                                     int maxPerMap, int nmsRadius)
{
    std::vector<Candidate> local;
    const int h = scoreMap.rows, w = scoreMap.cols;
    const float th = (float)thresh;
    for (int y = 0; y < h; y++)
    {
        const float* ps = scoreMap.ptr<float>(y);
        const float* pm = scoreMap.ptr<float>(std::max(0, y - 1));
        const float* pp = scoreMap.ptr<float>(std::min(h - 1, y + 1));
        for (int x = 0; x < w; x++)
        {
            float v = ps[x];
            if (v < th) continue;

            // 3x3 maximum inline instead of materialising a dilated copy of the
            // whole score map: out of range neighbours are clamped, which only
            // duplicates a value that is already part of the maximum (the same
            // result as cv::dilate with BORDER_CONSTANT).  Only the pixels above
            // the threshold pay for the eight neighbours, and the map sized
            // temporary plus its extra pass are gone.  Also note this stays
            // serial: cv::dilate would run its own parallel_for_ from inside
            // this parallel loop and corrupt the OpenCV thread pool.
            const int xm = std::max(0, x - 1);
            const int xp = std::min(w - 1, x + 1);
            float m = ps[x];
            m = std::max(m, ps[xm]); m = std::max(m, ps[xp]);
            m = std::max(m, pm[xm]); m = std::max(m, pm[x]);
            m = std::max(m, pm[xp]);
            m = std::max(m, pp[xm]); m = std::max(m, pp[x]);
            m = std::max(m, pp[xp]);
            if (v < m - 1e-5f) continue;

            // sub pixel peak position from a parabola through the neighbours,
            // this keeps the pyramid error from piling up while refining
            float dx = 0, dy = 0;
            if (x > 0 && x + 1 < w)
            {
                float a = ps[x - 1], b = ps[x + 1];
                float den = 2.f * v - a - b;
                if (den > 1e-6f) dx = std::min(1.f, std::max(-1.f, 0.5f * (b - a) / den));
            }
            if (y > 0 && y + 1 < h)
            {
                float a = scoreMap.ptr<float>(y - 1)[x], b = scoreMap.ptr<float>(y + 1)[x];
                float den = 2.f * v - a - b;
                if (den > 1e-6f) dy = std::min(1.f, std::max(-1.f, 0.5f * (b - a) / den));
            }

            Candidate c;
            c.level = level;
            c.pos = cv::Point2f((float)(x + R) + dx, (float)(y + R) + dy);
            c.angle = (float)angleGrid_[aIdx];
            c.scale = (float)scaleGrid_[sIdx];
            c.angleIdx = aIdx;
            c.scaleIdx = sIdx;
            c.score = v;
            local.push_back(c);
        }
    }
    if (local.empty()) return;
    std::sort(local.begin(), local.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if ((int)local.size() > maxPerMap) local.resize(maxPerMap);

    for (size_t i = 0; i < local.size(); i++)
    {
        const Candidate& c = local[i];
        bool keep = true;
        for (size_t j = 0; j < i; j++)
        {
            const Candidate& o = local[j];
            if (std::abs(c.pos.x - o.pos.x) <= nmsRadius &&
                std::abs(c.pos.y - o.pos.y) <= nmsRadius &&
                std::abs(c.angle - o.angle) <= 1e-3f + (float)levelAngleStep_[level] &&
                std::abs(c.scale - o.scale) <= 1e-3f + (float)levelScaleStep_[level] * 0.5f)
            {
                keep = false;
                break;
            }
        }
        if (keep) out.push_back(c);
    }
}

void ShapeMatcher::prepareSource(const cv::Mat& gray, int levels, bool useOCL)
{
    srcPyramid_.resize(levels);
    // An 8U input is used as-is: srcPyramid_[0] is only ever a SOURCE below
    // (pyrDown / gradientHalves), and findShapeModel() already hands us an 8U
    // buffer.  Copying here used to convertTo() every pixel a second time -
    // measured 1.5 ms on 2448x2048, i.e. 22% of the whole preprocess, for no
    // benefit.  A shallow reference also keeps the caller's buffer alive, which
    // is harmless because nothing here writes to it.
    if (gray.type() == CV_8U) srcPyramid_[0] = gray;
    else                      gray.convertTo(srcPyramid_[0], CV_8U);
    for (int i = 1; i < levels; i++)
        cv::pyrDown(srcPyramid_[i - 1], srcPyramid_[i]);

    srcGx_.resize(levels);
    srcGy_.resize(levels);
    srcQ8_.resize(levels);
    srcM8_.resize(levels);
    uGx_.resize(levels);
    uGy_.resize(levels);

    const double mc = params_.minContrast;
    // the 8-bit mirror has to carry the same skipWeight mask the scan uses
    const float w0 = skipWeight_;

    if (useOCL)
    {
        std::vector<cv::UMat> pyr(levels);
        for (int i = 0; i < levels; i++)
            srcPyramid_[i].copyTo(pyr[i]);
        for (int i = 0; i < levels; i++)
            normGradients<cv::UMat>(pyr[i], uGx_[i], uGy_[i], mc);
        for (int i = 0; i < levels; i++)
        {
            uGx_[i].copyTo(srcGx_[i]);
            uGy_[i].copyTo(srcGy_[i]);
            quantizeHalves(srcGx_[i], srcGy_[i], w0, srcQ8_[i], srcM8_[i]);
        }
    }
    else
    {
        for (int i = 0; i < levels; i++)
        {
            const cv::Mat& g8 = srcPyramid_[i];
            if (g8.cols > 8 && g8.rows > 8)
            {
                // one fused pass (sobel + normalize + CV_16F + 8-bit mirror)
                // instead of ten full image passes + two format conversions
                gradientHalves(g8, srcGx_[i], srcGy_[i], mc, w0, srcQ8_[i], srcM8_[i]);
            }
            else
            {
                // too small for the padded row buffers: use the OpenCV chain
                cv::Mat gx32, gy32;
                normGradients<cv::Mat>(g8, gx32, gy32, mc);
                gx32.convertTo(srcGx_[i], CV_16F);
                gy32.convertTo(srcGy_[i], CV_16F);
                quantizeHalves(srcGx_[i], srcGy_[i], w0, srcQ8_[i], srcM8_[i]);
            }
        }
    }
}

//=======================================================================
//  find_shape_model
//=======================================================================
std::vector<MatchResult> ShapeMatcher::findShapeModel(const cv::Mat& gray, const FindParams& fp)
{
    std::vector<MatchResult> results;
    if (!created_ || gray.empty())
        return results;

    cv::Mat g8;
    if (gray.channels() == 3) cv::cvtColor(gray, g8, cv::COLOR_BGR2GRAY);
    else if (gray.channels() == 4) cv::cvtColor(gray, g8, cv::COLOR_BGRA2GRAY);
    else if (gray.type() != CV_8U) gray.convertTo(g8, CV_8U);
    else g8 = gray;               // already 8U: no copy, prepareSource takes a reference

    const int nLevels = (int)modelLevels_.size();
    useOCL_ = fp.useOpenCL && oclUsable();

    // Halcon 的 NumLevels：搜索最多用模型的多少层（从最细层数起），0 = 全部。
    // 只影响"从哪层开始粗扫"，refine 仍然逐层走到 level 0，所以精度不变。
    // 传进来比模型层数大时按模型层数截断（Halcon 内部也是 capping）。
    int searchLevels = nLevels;
    if (fp.numLevels > 0) searchLevels = std::min(nLevels, fp.numLevels);
    if (searchLevels < 1) searchLevels = 1;
    // 只建到用得到的最高层：更粗的层没人读，建了就是白付预处理
    const int top = searchLevels - 1;

    // skipWeight（对齐 Halcon 的"弱梯度点不评分"语义）：本次调用的全部评分
    // 路径（SIMD 扫描/单点评分/亚像素/最终复评）都会读它。0 = 关闭。
    skipWeight_ = std::min(1.f, std::max(0.f, fp.skipWeight));

    int64 t0 = cv::getTickCount();
    prepareSource(g8, searchLevels, useOCL_);
    int64 t1 = cv::getTickCount();
    lastPreprocessTime_ = (double)(t1 - t0) * 1000.0 / cv::getTickFrequency();

    // phase timers for -debug 2 (top level scan, candidate extraction, the
    // per level refinement, sub-pixel fit and the optional re-scoring)
    double phTop = 0, phExtract = 0, phRefine = 0, phSubpixel = 0, phRescore = 0;
    double phMerge = 0;   // the serial sort + near-duplicate pass after the scan

    const int nA = (int)angleGrid_.size();
    const int nS = (int)scaleGrid_.size();

    //---- 搜索期角度/尺度子范围（Halcon find_shape_model 的 AngleStart/AngleExtent、
    //     find_scaled_shape_model 的 ScaleMin/ScaleMax）----
    // 只筛"从哪些位姿开始粗扫"，位姿本身仍取自模型的网格，因此不会产生新位姿。
    // 角度按 2*pi 取模，所以跨 ±180 的区间也能正确表达。
    timedOut_ = false;
    const int64 tDeadline = (fp.timeoutMs > 0)
        ? (cv::getTickCount() + (int64)((double)fp.timeoutMs * cv::getTickFrequency() / 1000.0))
        : 0;

    std::vector<int> selA, selS;
    selA.reserve(nA);
    selS.reserve(nS);
    {
        const bool angRestrict = (fp.angleExtent > 0.0);
        const double qs = fp.angleStart;
        const double qe = std::min(fp.angleExtent, 2 * CV_PI);
        const double qc = qs + qe * 0.5;                 // 请求范围中心
        int nearestA = 0;
        double nearestD = 1e30;
        for (int i = 0; i < nA; i++)
        {
            if (angRestrict)
            {
                // 归一化到 [0, 2pi) 后比较，天然处理跨越 ±pi 的区间
                double d = std::fmod(angleGrid_[i] - qs, 2 * CV_PI);
                if (d < 0) d += 2 * CV_PI;
                double dc = std::fmod(angleGrid_[i] - qc, 2 * CV_PI);
                if (dc > CV_PI) dc = 2 * CV_PI - dc;
                if (dc < nearestD) { nearestD = dc; nearestA = i; }
                if (d > qe + 1e-9) continue;
            }
            selA.push_back(i);
        }
        const bool scRestrict = (fp.scaleMax > 0.0);
        const double sLo = std::min(fp.scaleMin, fp.scaleMax);
        const double sHi = std::max(fp.scaleMin, fp.scaleMax);
        int nearestS = 0;
        double nearestDS = 1e30;
        const double sc = 0.5 * (sLo + sHi);
        for (int j = 0; j < nS; j++)
        {
            const double ds = std::fabs(scaleGrid_[j] - sc);
            if (ds < nearestDS) { nearestDS = ds; nearestS = j; }
            if (scRestrict && (scaleGrid_[j] < sLo - 1e-9 || scaleGrid_[j] > sHi + 1e-9)) continue;
            selS.push_back(j);
        }
        // 请求范围比本层网格步长还窄时一个点都落不进来。退回"全部"会让窄范围
        // 反而变慢，所以取离范围中心最近的那一个：粗层本就允许 ±1 个粗步长的
        // 误差，refine 会把它收敛回来。
        if (selA.empty()) selA.push_back(nearestA);
        if (selS.empty()) selS.push_back(nearestS);
    }
    const bool subRange = ((int)selA.size() != nA) || ((int)selS.size() != nS);

    // the model has to fit into the search image
    {
        const int Kt = 2 * getXForm(top, 0, 0).R + 1;
        if (srcGx_[top].cols < Kt + 1 || srcGx_[top].rows < Kt + 1)
        {
            lastSearchTime_ = 0.0;
            return results;
        }
    }
    const int strideA = 1 << top;
    const int strideS = 1 << top;

    // 在**选中的**索引列表上再抽稀，而不是在整个网格上按 1<<top 取模：
    // 只在整个网格上取模时，一旦请求范围窄于粗层角度步长（例如 360° 模型要 ±2°），
    // 可能一个粗层位姿都选不到。这里保证至少 2 个采样。
    const int kA = (int)selA.size(), kS = (int)selS.size();
    const int stepA = std::min(strideA, std::max(1, kA / 2));
    const int stepS = std::min(strideS, std::max(1, kS / 2));

    // at the coarsest level a lower score must be accepted
    const double topTarget = std::max(0.10, fp.minScore * 0.55);

    std::vector<Candidate> cands;
    const int64 tTop0 = cv::getTickCount();
    {
        std::vector<Candidate> all;
        cv::Mat map;
        double maxMap = -2;
        int nDense = 0;

        // all (angle, scale) poses of the coarsest level
        std::vector<std::pair<int, int>> poses;
        for (int js = 0; js < kS; js += stepS)
            for (int ja = 0; ja < kA; ja += stepA)
                poses.emplace_back(selA[ja], selS[js]);

        // 诊断探针（仅环境变量 SM_PROBE=1 时生效）：按角度打印粗层分数图的峰值
        // 及其位置，用来判断"正确的（大角度）位姿在粗层是否拿到了足够分数"。
        // 额外支持 SM_PROBE_X/SM_PROBE_Y（全分辨率坐标）：把该点在各角度下的
        // 粗层得分一起打印出来，这是定位"真实位姿在粗层就低分"的关键数据。
        // 正常运行时只多一次 getenv，对结果与耗时无影响。
        const bool probeCoarse = (std::getenv("SM_PROBE") != nullptr);
        bool probeHasPt = false;
        double probePx = 0.0, probePy = 0.0;
        double probeAng = 1e9;              // SM_PROBE_A: 真值角度(度)，可选
        if (probeCoarse)
        {
            const char* sx = std::getenv("SM_PROBE_X");
            const char* sy = std::getenv("SM_PROBE_Y");
            if (sx && sy)
            {
                probePx = std::atof(sx);
                probePy = std::atof(sy);
                probeHasPt = true;
            }
            const char* sa = std::getenv("SM_PROBE_A");
            if (sa) probeAng = std::atof(sa);
        }
        auto probeMap = [&](const cv::Mat& m, int ai, int R, int stride)
        {
            double pmn = 0, pmx = 0;
            cv::Point pmnl, pmxl;
            cv::minMaxLoc(m, &pmn, &pmx, &pmnl, &pmxl);
            std::cout << "  [probe] ang=" << angleGrid_[ai] * 180.0 / CV_PI
                      << " peak=" << pmx
                      << " at=(" << (pmxl.x + R) << "," << (pmxl.y + R) << ")";
            if (probeHasPt)
            {
                const int px = (int)std::lround(probePx / stride) - R;
                const int py = (int)std::lround(probePy / stride) - R;
                float pv = -2.f;
                if (px >= 0 && py >= 0 && px < m.cols && py < m.rows)
                    pv = m.at<float>(py, px);
                std::cout << " val=" << pv;
            }
            std::cout << "\n";
        };


        if (useOCL_)
        {
            // the dense path may build a correlation kernel inside the cache,
            // which is not thread safe -> keep this one serial
            for (size_t p = 0; p < poses.size(); p++)
            {
                if (tDeadline && cv::getTickCount() > tDeadline) { timedOut_ = true; break; }
                const int ai = poses[p].first, si = poses[p].second;
                int R = 0;
                const XForm& xf = getXForm(top, ai, si);
                const int K = 2 * xf.R + 1;
                // the dense path builds a correlation kernel, which cannot
                // express |dot|; any polarity other than use_polarity therefore
                // goes through the sparse scan
                if (fp.metric == 0 && skipWeight_ <= 0.f && K * K <= 6 * std::max(1, xf.n))
                {
                    correlate(top, ai, si, map, R);
                    nDense++;
                }
                else
                    scanPose(top, xf, map, R, topTarget, fp.greediness, fp.metric, false);
                if (fp.debug)
                {
                    double mn, mx;
                    cv::minMaxLoc(map, &mn, &mx);
                    if (mx > maxMap) maxMap = mx;
                }
                if (probeCoarse) probeMap(map, ai, R, strideA);
                int64 te0 = cv::getTickCount();
                extractCandidates(map, top, ai, si, R, topTarget, all, 64, 2);
                phExtract += msSince(te0);
            }
        }
        else
        {
            // warm the XForm cache up front.  warmXForm never evicts, so every
            // pose really is in the map before the threads start.
            for (size_t p = 0; p < poses.size(); p++)
                warmXForm(top, poses[p].first, poses[p].second);

            cv::Mutex mtx;
            cv::parallel_for_(cv::Range(0, (int)poses.size()),
                [&](const cv::Range& r)
                {
                    std::vector<Candidate> local;
                    cv::Mat lmap;
                    double localMax = -2;
                    double exMs = 0;
                    for (int i = r.start; i < r.end; i++)
                    {
                        // 超时：整个进程共用一个截止时刻，任意 worker 发现超时就停
                        if (tDeadline && cv::getTickCount() > tDeadline)
                        {
                            cv::AutoLock tl(mtx);
                            timedOut_ = true;
                            break;
                        }
                        const int ai = poses[i].first, si = poses[i].second;
                        int R = 0;
                        // read only lookup: a worker thread must never insert
                        // into the XForm cache.  If a pose somehow is not
                        // cached, build it locally instead of writing.
                        const XForm* pxf = findXForm(top, ai, si);
                        XForm localXf;
                        if (!pxf)
                        {
                            buildXForm(top, ai, si, localXf);
                            pxf = &localXf;
                            if (fp.debug)
                                fprintf(stderr, "  [dbg] cache miss in worker: "
                                        "ai=%d si=%d\n", ai, si);
                        }
                        scanPose(top, *pxf, lmap, R, topTarget,
                                 fp.greediness, fp.metric, true);
                        if (fp.debug)
                        {
                            double mn, mx;
                            minMaxSerial(lmap, mn, mx);
                            if (mx > localMax) localMax = mx;
                        }
                        if (probeCoarse)
                        {
                            cv::AutoLock plock(mtx);
                            probeMap(lmap, ai, R, strideA);
                        }
                        int64 te0 = cv::getTickCount();
                        // SM_NO_EXTRACT=1 skips the local-maximum pass, to
                        // measure what it actually costs on the wall clock
                        // (phExtract sums every worker, so it is not that).
                        static const bool noExtract = (std::getenv("SM_NO_EXTRACT") != nullptr);
                        if (!noExtract)
                            extractCandidates(lmap, top, ai, si, R, topTarget, local, 64, 2);
                        exMs += msSince(te0);
                    }
                    cv::AutoLock lock(mtx);
                    all.insert(all.end(), local.begin(), local.end());
                    if (localMax > maxMap) maxMap = localMax;
                    phExtract += exMs;
                },
                // One stripe per pose.  With "poses/8" a 9-pose search got a
                // single stripe and ran fully serial: topScan stayed at 10.2 ms
                // whether cv::setNumThreads was 1 or 20.  Each pose is already a
                // long serial task (a full score map), so splitting finer only
                // helps: the OpenCV pool hands one pose to each worker.
                // SM_COARSE_STRIPES=0 restores the old "poses/8" grouping, so the
                // two schedulings can be A/B compared on the same binary.
                coarseStripes(poses.size()));
        }
        if (fp.debug)
        {
            std::cout << "  [dbg] levels=" << nLevels << " top=" << top
                      << " searchLevels=" << searchLevels
                      << " poses=" << poses.size() << " (A " << selA.size() << "/" << nA
                      << " x S " << selS.size() << "/" << nS << ")"
                      << (subRange ? " [sub-range]" : "")
                      << (timedOut_ ? " [TIMEOUT]" : "")
                      << " topTarget=" << topTarget
                      << " angles=" << nA << " scales=" << nS
                      << " dense=" << nDense
                      << " mapMax=" << maxMap
                      << " candidates=" << all.size() << "\n";
        }
        // The parallel scan is over now; everything below (sort + the O(n^2)
        // near-duplicate pass) runs on ONE thread, so it sits on the Amdahl
        // critical path and must be visible separately from the scan itself.
        const int64 tMerge0 = cv::getTickCount();
        std::sort(all.begin(), all.end(),
                  [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        int keep = std::max(fp.maxMatches * 4, 40);

        // 先做一次全局去重（位置/角度/尺度近似的只留最高分），得到干净的候选池。
        std::vector<Candidate> uniq;
        uniq.reserve(all.size());
        for (size_t i = 0; i < all.size(); i++)
        {
            const Candidate& c = all[i];
            bool dup = false;
            for (size_t j = 0; j < uniq.size(); j++)
            {
                const Candidate& o = uniq[j];
                if (std::abs(c.pos.x - o.pos.x) <= 3.f && std::abs(c.pos.y - o.pos.y) <= 3.f &&
                    angleDiffWrapped(c.angle, o.angle) <= 1e-4f + (float)levelAngleStep_[top] &&
                    std::abs(c.scale - o.scale) <= 1e-4f + (float)levelScaleStep_[top] * 0.5f)
                {
                    dup = true; break;
                }
            }
            if (!dup) uniq.push_back(c);
        }

        // 候选池按"全局分数"截断。
        // 说明：一度考虑过给候选加"角度桶多样性配额"（担心高对称目标在错误朝向上
        // 拿到更高粗层分、把真实大角度位姿挤出池子）。2026-11-14 在 data2 实测否定了
        // 这个假设：用一个按真实朝向对齐的模板另建模型，正确位姿的最终得分也只有
        // 0.29~0.36（Halcon 0.57~0.68），说明"漏检"的根因是模板与目标实例的外观差异
        // （模板单孔、目标双孔），不是候选排序。因此保留最简单的全局截断，零回归。
        if (probeHasPt)
        {
            // 诊断：列出与真值位置/朝向相关的候选在 uniq 里的排名，用来确认
            // "正确位姿是否在全局截断之前就已经排在 keep 之外"。
            const int lx = (int)std::lround(probePx / strideA);
            const int ly = (int)std::lround(probePy / strideA);
            int shown = 0;
            std::cout << "  [dbg] uniq total=" << uniq.size() << " keep=" << keep
                      << " probeLevelPos=(" << lx << "," << ly << ")\n";
            for (size_t i = 0; i < uniq.size() && shown < 24; i++)
            {
                const Candidate& c = uniq[i];
                const bool nearPos = std::abs(c.pos.x - lx) <= 3 && std::abs(c.pos.y - ly) <= 3;
                const bool nearAng = std::abs(probeAng - c.angle * 180.0 / CV_PI) <= 10.0;
                if (!nearPos && !nearAng) continue;
                std::cout << "  [dbg] uniq#" << i << " score=" << c.score
                          << " ang=" << c.angle * 180.0 / CV_PI
                          << " pos=(" << (int)(c.pos.x * strideA) << ","
                          << (int)(c.pos.y * strideA) << ")"
                          << (nearPos ? " <near-truth-pos>" : "") << "\n";
                shown++;
            }
        }
        // 全局按分截断（与旧版行为一致）
        for (size_t i = 0; i < uniq.size() && (int)cands.size() < keep; i++)
            cands.push_back(uniq[i]);

        phMerge = msSince(tMerge0);
        if (fp.debug)
            std::cout << "  [dbg] merge(sort+dedup, serial) = " << phMerge
                      << " ms   all=" << all.size() << " uniq=" << uniq.size()
                      << " keep=" << keep << "\n";
    }
    phTop = msSince(tTop0);

    const int maxKeep = std::max(fp.maxMatches * 3, 30);
    for (int l = top - 1; l >= 0; l--)
    {
        if (tDeadline && cv::getTickCount() > tDeadline) { timedOut_ = true; break; }
        int64 tr0 = cv::getTickCount();
        refineLevel(l, cands, maxKeep, fp.minScore, fp.metric, tDeadline, &timedOut_);
        phRefine += msSince(tr0);
        if (fp.debug && !cands.empty())
        {
            std::cout << "  [dbg] level " << l << ": n=" << cands.size()
                      << " best=" << cands[0].score
                      << " at (" << cands[0].pos.x << "," << cands[0].pos.y
                      << ") a=" << cands[0].angle * 180 / CV_PI
                      << " s=" << cands[0].scale << "\n";
        }
    }

    if (fp.subpixel)
    {
        int64 tsub0 = cv::getTickCount();
        for (size_t i = 0; i < cands.size(); i++)
            subpixelRefine(0, cands[i], fp.subpixel, fp.metric);
        phSubpixel = msSince(tsub0);
    }

    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    // Overlap between two instances, for maxOverlap.  Both are the same rotated
    // rectangle (template size x its own scale), so this is
    //   intersection area / area of the smaller rectangle,
    // 0 = no overlap, 1 = one completely contains the other.
    const cv::Size2f tplSize((float)modelLevels_[0].size.width,
                             (float)modelLevels_[0].size.height);
    auto overlapOf = [&](const MatchResult& a, const MatchResult& b) -> double
    {
        if (tplSize.width <= 0.f || tplSize.height <= 0.f) return 0.0;
        const cv::Size2f sa(tplSize.width * a.scale, tplSize.height * a.scale);
        const cv::Size2f sb(tplSize.width * b.scale, tplSize.height * b.scale);
        // the pose maps the model by [[ca, sa], [-sa, ca]], i.e. a rotation of
        // -angle; RotatedRect counts the angle the other way round in an image
        cv::RotatedRect ra(a.pos, sa, -(double)a.angle * 180.0 / CV_PI);
        cv::RotatedRect rb(b.pos, sb, -(double)b.angle * 180.0 / CV_PI);
        std::vector<cv::Point2f> inter;
        const int rc = cv::rotatedRectangleIntersection(ra, rb, inter);
        const double smaller = std::min((double)sa.area(), (double)sb.area());
        if (rc == cv::INTERSECT_NONE || inter.size() < 3 || smaller <= 0.0)
            return 0.0;
        return std::min(1.0, std::fabs(cv::contourArea(inter)) / smaller);
    };
    const bool useOverlap = (fp.maxOverlap < 0.999);

    const float minD = (float)std::max(1.0, fp.minDistance);

    // Pose detail (coverage + 4x4 missShare) at an arbitrary found pose.
    // Used twice: while collecting the result list (so a rejected candidate is
    // replaced by the next one instead of leaving a hole) and in the final
    // re-scoring pass.
    auto poseDetailAt = [&](const cv::Point2f& pos, float angle, float scale) -> PoseScore
    {
        int ai = 0, si = 0;
        double best = 1e9;
        for (int i = 0; i < (int)angleGrid_.size(); i++)
        {
            double d = std::abs(angleGrid_[i] - (double)angle);
            if (d < best) { best = d; ai = i; }
        }
        best = 1e9;
        for (int i = 0; i < (int)scaleGrid_.size(); i++)
        {
            double d = std::abs(scaleGrid_[i] - (double)scale);
            if (d < best) { best = d; si = i; }
        }
        XForm& xf = getXForm(0, ai, si);
        int ax = cvRound(pos.x), ay = cvRound(pos.y);
        ax = std::min(std::max(ax, xf.R), srcGx_[0].cols - 1 - xf.R);
        ay = std::min(std::max(ay, xf.R), srcGx_[0].rows - 1 - xf.R);
        return scorePoseDetail(srcGx_[0], srcGy_[0], xf, ay, ax, fp.metric, skipWeight_);
    };

    for (size_t i = 0; i < cands.size() && (int)results.size() < fp.maxMatches; i++)
    {
        const Candidate& c = cands[i];
        if (c.score < fp.minScore) break;
        bool dup = false;
        for (size_t j = 0; j < results.size(); j++)
        {
            const MatchResult& o = results[j];
            if (cv::norm(c.pos - o.pos) < minD &&
                angleDiffWrapped(c.angle, o.angle) < 0.35 &&
                std::abs(c.scale - o.scale) < 0.12)
            {
                dup = true; break;
            }
            if (useOverlap)
            {
                MatchResult candRes;
                candRes.pos = c.pos; candRes.angle = c.angle; candRes.scale = c.scale;
                if (overlapOf(candRes, o) > fp.maxOverlap) { dup = true; break; }
            }
        }
        if (dup) continue;

        // 模板缺失面积过大 -> 不是真目标（重复纹理里"只对上模板一部分"的错位
        // 实例）。必须在这里就拒，而不是等 results 收满再过滤：那样被拒的 top-1
        // 会让整张图变成 0 命中，后面的真实候选永远没有机会（实测 data2 有一帧
        // 就是这样从"能检出"退化成"漏检"）。
        if (fp.cellMissMax > 0.f)
        {
            const PoseScore ps = poseDetailAt(c.pos, c.angle, c.scale);
            if (ps.missShare > fp.cellMissMax) continue;
        }

        MatchResult m;
        m.pos = c.pos;
        m.angle = c.angle;
        m.scale = c.scale;
        m.score = c.score;
        results.push_back(m);
    }

    //---- robust re-scoring of the final matches --------------------------
    // the search / refine used the magnitude-damped dot product for ranking;
    // the score that is *reported* must be the one a user compares with Halcon:
    // mean over all model points of the direction cosine ("scoreMode 0").
    // This pass also yields coverage (fraction of agreeing points) and is the
    // only place the reported value is produced, so it always runs.
    // Cost: one n-point pass per reported match.
    //
    // The "template cell missing" verdict (missShare) is already applied while
    // collecting the results (see above), so every match reaching this pass has
    // passed cellMissMax.
    if (!results.empty())
    {
        int64 trs0 = cv::getTickCount();
        for (size_t ri = 0; ri < results.size(); ri++)
        {
            MatchResult& r = results[ri];
            const PoseScore ps = poseDetailAt(r.pos, r.angle, r.scale);
            r.coverage = ps.coverage;
            if (fp.scoreMode == 2)          // trimmed-mean needs the sorted dots
            {
                int ai = 0, si = 0;
                double best = 1e9;
                for (int i = 0; i < (int)angleGrid_.size(); i++)
                {
                    double d = std::abs(angleGrid_[i] - (double)r.angle);
                    if (d < best) { best = d; ai = i; }
                }
                best = 1e9;
                for (int i = 0; i < (int)scaleGrid_.size(); i++)
                {
                    double d = std::abs(scaleGrid_[i] - (double)r.scale);
                    if (d < best) { best = d; si = i; }
                }
                XForm& xf = getXForm(0, ai, si);
                int ax = cvRound(r.pos.x), ay = cvRound(r.pos.y);
                ax = std::min(std::max(ax, xf.R), srcGx_[0].cols - 1 - xf.R);
                ay = std::min(std::max(ay, xf.R), srcGx_[0].rows - 1 - xf.R);
                r.score = scorePosMode(srcGx_[0], srcGy_[0], xf, ay, ax, 2, fp.metric);
            }
            else if (fp.scoreMode == 0)     // mean: identical to the search score
                r.score = ps.mean;
            else if (fp.scoreMode == 1)     // positive-mean
            {
                // local polarity makes every point positive, so coverage is not
                // a useful denominator there
                r.score = (fp.metric == 2 || ps.coverage <= 0.f)
                              ? ps.mean
                              : ps.clipped / ps.coverage;
            }
            else                            // any other mode, i.e. 3 = clipped-mean
                r.score = ps.clipped;
        }

        std::sort(results.begin(), results.end(),
                  [](const MatchResult& a, const MatchResult& b)
                  { return a.score > b.score; });

        // drop the matches that only cover part of the model.  Runs after the
        // sort so the survivors stay ordered; a match that fails here is not
        // replaced by a lower-ranked one, so results can be fewer than
        // maxMatches - which is the whole point of the filter.
        if (fp.minCoverage > 0.f)
        {
            std::vector<MatchResult> keep;
            keep.reserve(results.size());
            for (const MatchResult& r : results)
                if (r.coverage >= fp.minCoverage) keep.push_back(r);
            results.swap(keep);
        }
        // Re-apply the score threshold to the *final* score.  Until now only the
        // candidate score was checked (`if (c.score < minScore) break;` while
        // collecting); a different score mode can lower the reported value below
        // the user's minScore, and such a match would still be listed with a
        // printed score that contradicts the setting.
        if (fp.minScore > 0.f)
        {
            std::vector<MatchResult> keep;
            keep.reserve(results.size());
            for (const MatchResult& r : results)
                if (r.score >= fp.minScore) keep.push_back(r);
            results.swap(keep);
        }
        phRescore = msSince(trs0);
    }

    if (fp.debug >= 2)
    {
        // where the search time actually goes (debug >= 2)
        std::cout << "  [dbg2] topScan=" << phTop << " (extract=" << phExtract
                  << " sum-over-workers, merge(sort+dedup,serial)=" << phMerge
                  << ", scan=" << (phTop - phMerge) << ")"
                  << " refine=" << phRefine << " subpixel=" << phSubpixel
                  << " rescore=" << phRescore
                  << "  sum=" << (phTop + phRefine + phSubpixel + phRescore)
                  << " ms  poses=" << (((nA + strideA - 1) / strideA) *
                                        ((nS + strideS - 1) / strideS))
                  << " keep=" << cands.size() << "\n";
    }

    //---- per-point dot histogram at the top-1 pose (diagnostics) ----------
    // Set SM_DUMP_COS=1 to print, for result #0, the distribution of the
    // per-model-point gradient-direction dot products at the reported pose.
    // This shows whether a low mean score comes from (a) a subset of flipped/
    // noisy points (bimodal histogram) or (b) a systematic direction offset
    // (single broad peak) - which call for very different fixes.
    if (results.empty() == false && std::getenv("SM_DUMP_COS") != nullptr)
    {
        const MatchResult& r0 = results.front();
        int ai = 0, si = 0;
        double best = 1e9;
        for (int i = 0; i < (int)angleGrid_.size(); i++)
        {
            double d = std::abs(angleGrid_[i] - (double)r0.angle);
            if (d < best) { best = d; ai = i; }
        }
        best = 1e9;
        for (int i = 0; i < (int)scaleGrid_.size(); i++)
        {
            double d = std::abs(scaleGrid_[i] - (double)r0.scale);
            if (d < best) { best = d; si = i; }
        }
        const XForm& xf = getXForm(0, ai, si);
        int ax = cvRound(r0.pos.x), ay = cvRound(r0.pos.y);
        ax = std::min(std::max(ax, xf.R), srcGx_[0].cols - 1 - xf.R);
        ay = std::min(std::max(ay, xf.R), srcGx_[0].rows - 1 - xf.R);

        std::vector<float> dots((size_t)xf.n);
        for (int k = 0; k < xf.n; k++)
        {
            const float gx = sampleF16Bilinear(srcGx_[0], (float)ay + xf.fy[k], (float)ax + xf.fx[k]);
            const float gy = sampleF16Bilinear(srcGy_[0], (float)ay + xf.fy[k], (float)ax + xf.fx[k]);
            dots[(size_t)k] = xf.nx[k] * gx + xf.ny[k] * gy;
        }
        std::sort(dots.begin(), dots.end());
        // 12 buckets over [-1, 1]
        int hist[12] = { 0 };
        int neg = 0;
        for (int k = 0; k < xf.n; k++)
        {
            float d = dots[(size_t)k];
            if (d < 0.f) neg++;
            int b = (int)std::floor((d + 1.f) * 6.f);
            b = std::min(11, std::max(0, b));
            hist[b]++;
        }
        auto q = [&](float p)
        {
            int idx = std::min(xf.n - 1, std::max(0, (int)std::lround(p * (xf.n - 1))));
            return dots[(size_t)idx];
        };
        std::cout << "  [cosdump] n=" << xf.n
                  << " p5=" << q(0.05f) << " p25=" << q(0.25f)
                  << " p50=" << q(0.50f) << " p75=" << q(0.75f)
                  << " p95=" << q(0.95f) << " negShare=" << (float)neg / xf.n << "\n";
        std::cout << "  [cosdump] hist (0=-1.0 .. 11=+1.0): ";
        for (int b = 0; b < 12; b++) std::cout << hist[b] << " ";
        std::cout << "\n";
    }

    int64 t2 = cv::getTickCount();
    lastSearchTime_ = (double)(t2 - t1) * 1000.0 / cv::getTickFrequency();
    return results;
}

//=======================================================================
//  probe: score of one exact pose
//=======================================================================
double ShapeMatcher::probe(const cv::Mat& gray, cv::Point2f pos, double angle, double scale)
{
    if (!created_ || gray.empty()) return -1;
    cv::Mat g8;
    if (gray.channels() == 3) cv::cvtColor(gray, g8, cv::COLOR_BGR2GRAY);
    else gray.convertTo(g8, CV_8U);
    if (srcGx_.empty() || srcGx_[0].cols != g8.cols)
    {
        useOCL_ = false;
        prepareSource(g8, (int)modelLevels_.size(), false);
    }

    int ai = 0, si = 0;
    double best = 1e9;
    for (int i = 0; i < (int)angleGrid_.size(); i++)
    {
        double d = std::abs(angleGrid_[i] - angle);
        if (d < best) { best = d; ai = i; }
    }
    best = 1e9;
    for (int i = 0; i < (int)scaleGrid_.size(); i++)
    {
        double d = std::abs(scaleGrid_[i] - scale);
        if (d < best) { best = d; si = i; }
    }

    XForm& xf = getXForm(0, ai, si);
    int ax = cvRound(pos.x), ay = cvRound(pos.y);
    ax = std::min(std::max(ax, xf.R), srcGx_[0].cols - 1 - xf.R);
    ay = std::min(std::max(ay, xf.R), srcGx_[0].rows - 1 - xf.R);
    return scorePos(srcGx_[0], srcGy_[0], xf, ay, ax, -10.f, POL_USE, skipWeight_);
}

//=======================================================================
//  drawing helpers
//=======================================================================
void ShapeMatcher::drawContours(cv::Mat& dst, cv::Scalar color, int thickness) const
{
    if (!created_ || dst.empty()) return;
    if (dst.channels() == 1) cv::cvtColor(dst, dst, cv::COLOR_GRAY2BGR);
    const LevelData& ld = modelLevels_[0];
    for (size_t k = 0; k < ld.points.size(); k++)
    {
        const ModelPoint& mp = ld.points[k];
        int x = cvRound(mp.x + ld.center.x);
        int y = cvRound(mp.y + ld.center.y);
        if (x < 0 || y < 0 || x >= dst.cols || y >= dst.rows) continue;
        dst.at<cv::Vec3b>(y, x) = cv::Vec3b((uchar)color[0], (uchar)color[1], (uchar)color[2]);
    }
    (void)thickness;
}

void ShapeMatcher::drawContours(cv::Mat& dst, const MatchResult& m, cv::Scalar color, int thickness) const
{
    if (!created_ || dst.empty()) return;
    if (dst.channels() == 1) cv::cvtColor(dst, dst, cv::COLOR_GRAY2BGR);

    const LevelData& ld = modelLevels_[0];
    const float ca = (float)std::cos(m.angle), sa = (float)std::sin(m.angle);
    const float s = m.scale;

    std::vector<cv::Point> pts;
    pts.reserve(ld.points.size());
    for (size_t k = 0; k < ld.points.size(); k++)
    {
        const ModelPoint& mp = ld.points[k];
        float x = s * (mp.x * ca + mp.y * sa) + m.pos.x;
        float y = s * (-mp.x * sa + mp.y * ca) + m.pos.y;
        pts.push_back(cv::Point(cvRound(x), cvRound(y)));
    }
    std::vector<cv::Point> hull;
    cv::convexHull(pts, hull);
    cv::polylines(dst, hull, true, color, thickness);
    cv::circle(dst, cv::Point(cvRound(m.pos.x), cvRound(m.pos.y)), 3, color, -1);
}

cv::Point2f ShapeMatcher::modelCenter() const
{
    return created_ ? modelLevels_[0].center : cv::Point2f();
}

int ShapeMatcher::modelPointCount() const
{
    return created_ ? modelLevels_[0].activePoints : 0;
}

//=======================================================================
//  persistence: write_shape_model / read_shape_model
//
//  Layout (little endian, all fields written one by one so the file does
//  not depend on struct padding):
//    header   : magic 'SHM1' | uint32 version
//    params   : MatchParams (numLevels, angle*, scale*, contrast*,
//               maxPoints, minScore, maxMatches, minDistance, subpixel,
//               minSide from version 4 on,
//               optimization + contrastBands from version 5 on)
//    modelGray0_
//    levels   : uint32 nLevels, then per level
//                 Size | Point2f center | int activePoints | uint32 n
//                 then n x ModelPoint (5 x float32)
//    grids    : angleGrid_ | scaleGrid_ | angleStep_ scaleStep_ scaleMin_
//               levelAngleStep_ levelScaleStep_
//               levelAngleCount_ levelScaleCount_
//    chains   : (version >= 3) uint32 nChains, then per chain
//                 uint32 len | len x (int16 x, int16 y) relative to centre
//               Only level 0 has chains; they are display-only contours (the
//               full edge polylines, Halcon's get_shape_model_contours).
//               Version 1 (no chains) and 2 are still readable.
//=======================================================================
namespace
{
    const uint32_t SM_MAGIC   = 0x314D4853;   // 'SHM1'
    // 4: MatchParams 末尾追加 minSide（金字塔最粗层最短边下限）。
    //    读 v1..v3 的老模型时该字段回落 MatchParams 的默认 24 —— 正好是当时
    //    硬编码的值，所以老模型的层数和行为都不变。
    // 5: 追加 optimization（int32 编码）+ contrastBands（uint32 对数 + f64 对）。
    //    读 v1..v4 时两者保持默认（空 = 历史行为）。
    const uint32_t SM_VERSION = 5;
    const uint32_t SM_VERSION_MIN = 1;        // oldest layout we can read

    struct ByteWriter
    {
        std::vector<uchar> b;
        void raw(const void* p, size_t n)
        {
            const uchar* s = (const uchar*)p;
            b.insert(b.end(), s, s + n);
        }
        void u32(uint32_t v) { raw(&v, 4); }
        void i32(int32_t  v) { raw(&v, 4); }
        void i16(int16_t  v) { raw(&v, 2); }
        void f32(float    v) { raw(&v, 4); }
        void f64(double   v) { raw(&v, 8); }
    };

    struct ByteReader
    {
        const uchar* p = nullptr;
        size_t n = 0, pos = 0;
        bool ok = true;

        bool raw(void* dst, size_t k)
        {
            if (!ok) return false;
            if (pos + k > n) { ok = false; return false; }
            if (dst) std::memcpy(dst, p + pos, k);
            pos += k;
            return true;
        }
        bool u32(uint32_t& v) { return raw(&v, 4); }
        bool i32(int32_t&  v) { return raw(&v, 4); }
        bool i16(int16_t&  v) { return raw(&v, 2); }
        bool f32(float&    v) { return raw(&v, 4); }
        bool f64(double&   v) { return raw(&v, 8); }
        bool done() const { return ok; }
    };
}

std::vector<uchar> ShapeMatcher::serializeShapeModel() const
{
    if (!created_) return std::vector<uchar>();

    ByteWriter w;
    w.u32(SM_MAGIC);
    w.u32(SM_VERSION);

    // ---- MatchParams ----
    w.i32(params_.numLevels);
    w.f64(params_.angleStart);
    w.f64(params_.angleExtent);
    w.f64(params_.angleStep);
    w.f64(params_.scaleMin);
    w.f64(params_.scaleMax);
    w.f64(params_.scaleStep);
    w.f64(params_.contrast);
    w.f64(params_.minContrast);
    w.i32(params_.maxPoints);
    w.f64(params_.minScore);
    w.i32(params_.maxMatches);
    w.f64(params_.minDistance);
    w.i32(params_.subpixel);
    // version >= 4
    w.i32(params_.minSide);
    // version >= 5: optimization（0 = 空/none, 1 = auto, 2/3/4 = low/medium/high）
    int opt = 0;
    if      (params_.optimization == "auto")                  opt = 1;
    else if (params_.optimization == "point_reduction_low")    opt = 2;
    else if (params_.optimization == "point_reduction_medium") opt = 3;
    else if (params_.optimization == "point_reduction_high")   opt = 4;
    w.i32(opt);
    // version >= 5: contrastBands
    w.u32((uint32_t)(params_.contrastBands.size() / 2));
    for (size_t i = 0; i + 1 < params_.contrastBands.size(); i += 2)
    {
        w.f64(params_.contrastBands[i]);
        w.f64(params_.contrastBands[i + 1]);
    }

    // ---- modelGray0_ (CV_8U, continuous) ----
    cv::Mat mg = modelGray0_.isContinuous() ? modelGray0_ : modelGray0_.clone();
    w.i32(mg.rows);
    w.i32(mg.cols);
    w.i32(mg.type());
    w.raw(mg.data, (size_t)mg.rows * (size_t)mg.cols * (size_t)mg.elemSize());

    // ---- levels ----
    w.u32((uint32_t)modelLevels_.size());
    for (size_t l = 0; l < modelLevels_.size(); l++)
    {
        const LevelData& ld = modelLevels_[l];
        w.i32(ld.size.width);
        w.i32(ld.size.height);
        w.f32(ld.center.x);
        w.f32(ld.center.y);
        w.i32(ld.activePoints);
        w.u32((uint32_t)ld.points.size());
        for (size_t k = 0; k < ld.points.size(); k++)
        {
            const ModelPoint& mp = ld.points[k];
            w.f32(mp.x); w.f32(mp.y);
            w.f32(mp.gx); w.f32(mp.gy);
            w.f32(mp.mag);
        }
    }

    // ---- grids ----
    w.u32((uint32_t)angleGrid_.size());
    for (size_t i = 0; i < angleGrid_.size(); i++) w.f64(angleGrid_[i]);
    w.u32((uint32_t)scaleGrid_.size());
    for (size_t i = 0; i < scaleGrid_.size(); i++) w.f64(scaleGrid_[i]);
    w.f64(angleStep_);
    w.f64(scaleStep_);
    w.f64(scaleMin_);

    w.u32((uint32_t)levelAngleStep_.size());
    for (size_t i = 0; i < levelAngleStep_.size(); i++) w.f64(levelAngleStep_[i]);
    for (size_t i = 0; i < levelScaleStep_.size(); i++) w.f64(levelScaleStep_[i]);
    for (size_t i = 0; i < levelAngleCount_.size(); i++) w.i32(levelAngleCount_[i]);
    for (size_t i = 0; i < levelScaleCount_.size(); i++) w.i32(levelScaleCount_[i]);

    // ---- chains (level 0 only), version 3: contour polylines ----
    const std::vector<std::vector<cv::Point>> noChains;
    const size_t nLev = modelLevels_.size();
    const std::vector<std::vector<cv::Point>>& ch0 = nLev > 0 ? modelLevels_[0].chains : noChains;
    w.u32((uint32_t)ch0.size());
    for (size_t c = 0; c < ch0.size(); c++)
    {
        w.u32((uint32_t)ch0[c].size());
        for (size_t k = 0; k < ch0[c].size(); k++)
        {
            w.i16((int16_t)ch0[c][k].x);
            w.i16((int16_t)ch0[c][k].y);
        }
    }

    return w.b;
}

bool ShapeMatcher::deserializeShapeModel(const std::vector<uchar>& data)
{
    ByteReader r;
    r.p = data.empty() ? nullptr : &data[0];
    r.n = data.size();

    uint32_t magic = 0, version = 0;
    r.u32(magic); r.u32(version);
    if (magic != SM_MAGIC || version < SM_VERSION_MIN || version > SM_VERSION)
        return false;

    MatchParams p;
    r.i32(p.numLevels);
    r.f64(p.angleStart); r.f64(p.angleExtent); r.f64(p.angleStep);
    r.f64(p.scaleMin);   r.f64(p.scaleMax);    r.f64(p.scaleStep);
    r.f64(p.contrast);   r.f64(p.minContrast);
    r.i32(p.maxPoints);
    r.f64(p.minScore);
    r.i32(p.maxMatches);
    r.f64(p.minDistance);
    r.i32(p.subpixel);
    // version >= 4; older models keep the default (24 = the value that used to
    // be hard coded, so their level count stays exactly the same)
    if (version >= 4) r.i32(p.minSide);
    // version >= 5: optimization + contrastBands（老模型保持默认 = 历史行为）
    if (version >= 5)
    {
        int opt = 0;
        r.i32(opt);
        switch (opt)
        {
            case 1: p.optimization = "auto"; break;
            case 2: p.optimization = "point_reduction_low"; break;
            case 3: p.optimization = "point_reduction_medium"; break;
            case 4: p.optimization = "point_reduction_high"; break;
            default: break;                 // 0 / 非法 -> 空
        }
        uint32_t nBands = 0;
        r.u32(nBands);
        if (nBands > 64) return false;      // 防损坏文件
        p.contrastBands.resize((size_t)nBands * 2);
        for (uint32_t i = 0; i < nBands; i++)
        {
            double a = 0, b = 0;
            r.f64(a); r.f64(b);
            p.contrastBands[(size_t)i * 2]     = a;
            p.contrastBands[(size_t)i * 2 + 1] = b;
        }
    }

    int rows = 0, cols = 0, type = 0;
    r.i32(rows); r.i32(cols); r.i32(type);
    if (rows <= 0 || cols <= 0) return false;
    cv::Mat mg = cv::Mat::zeros(rows, cols, type);
    r.raw(mg.data, (size_t)rows * (size_t)cols * (size_t)mg.elemSize());

    uint32_t nLevels = 0;
    r.u32(nLevels);
    if (nLevels == 0 || nLevels > 8) return false;
    std::vector<LevelData> levels(nLevels);
    for (uint32_t l = 0; l < nLevels; l++)
    {
        LevelData& ld = levels[l];
        r.i32(ld.size.width); r.i32(ld.size.height);
        r.f32(ld.center.x);   r.f32(ld.center.y);
        r.i32(ld.activePoints);
        uint32_t np = 0;
        r.u32(np);
        ld.points.resize(np);
        for (uint32_t k = 0; k < np; k++)
        {
            ModelPoint& mp = ld.points[k];
            r.f32(mp.x); r.f32(mp.y);
            r.f32(mp.gx); r.f32(mp.gy);
            r.f32(mp.mag);
        }
    }

    uint32_t nAng = 0, nSca = 0, ng = 0;
    r.u32(nAng);
    std::vector<double> ag(nAng);
    for (uint32_t i = 0; i < nAng; i++) r.f64(ag[i]);
    r.u32(nSca);
    std::vector<double> sg(nSca);
    for (uint32_t i = 0; i < nSca; i++) r.f64(sg[i]);

    double aStep = 0, sStep = 0, sMin = 1;
    r.f64(aStep); r.f64(sStep); r.f64(sMin);

    r.u32(ng);
    std::vector<double> las(ng), lss(ng);
    std::vector<int>    lac(ng), lsc(ng);
    for (uint32_t i = 0; i < ng; i++) r.f64(las[i]);
    for (uint32_t i = 0; i < ng; i++) r.f64(lss[i]);
    for (uint32_t i = 0; i < ng; i++) r.i32(lac[i]);
    for (uint32_t i = 0; i < ng; i++) r.i32(lsc[i]);

    // sanity: the per level tables must match the level count
    if (!r.done() || ng != nLevels || levels[0].points.empty())
        return false;

    // ---- chains (version >= 3, level 0 only) ----
    if (version >= 3)
    {
        uint32_t nc = 0;
        if (r.u32(nc) && nc < 2000000u)          // sanity against a corrupt count
        {
            levels[0].chains.resize(nc);
            bool bad = false;
            for (uint32_t c = 0; c < nc && r.done(); c++)
            {
                uint32_t len = 0;
                if (!r.u32(len) || len > 4000000u) { bad = true; break; }
                levels[0].chains[c].resize(len);
                for (uint32_t k = 0; k < len; k++)
                {
                    int16_t x = 0, y = 0;
                    if (!r.i16(x) || !r.i16(y)) { bad = true; break; }
                    levels[0].chains[c][k] = cv::Point(x, y);
                }
                if (bad) break;
            }
            if (bad) levels[0].chains.clear();     // contours are optional: drop them
        }
    }

    // ---- commit ----
    params_        = p;
    modelGray0_    = mg;
    modelLevels_   = levels;
    angleGrid_     = ag;
    scaleGrid_     = sg;

    // the point list is stored strongest first, so the two ends of it give
    // the same tuning hint as right after createShapeModel().  the number of
    // candidate pixels is not part of the file, 0 means "unknown".
    candCount_    = 0;
    strongestMag_ = levels[0].points.front().mag;
    weakestMag_   = levels[0].points.back().mag;
    // geometry facts are derived from the points, so they can be rebuilt from
    // the file even though they are not stored in it
    computeGeometryFacts(levels[0]);
    angleStep_     = aStep;
    scaleStep_     = sStep;
    scaleMin_      = sMin;
    levelAngleStep_  = las;
    levelScaleStep_  = lss;
    levelAngleCount_ = lac;
    levelScaleCount_ = lsc;

    // the XForm cache belongs to the previous model
    impl_->xformCache.clear();
    srcPyramid_.clear(); srcGx_.clear(); srcGy_.clear();
    uGx_.clear(); uGy_.clear();
    useOCL_ = false;
    created_ = true;
    return true;
}

bool ShapeMatcher::writeShapeModel(const std::string& filename) const
{
    std::vector<uchar> buf = serializeShapeModel();
    if (buf.empty()) return false;

    std::ofstream f(filename.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)&buf[0], (std::streamsize)buf.size());
    return f.good();
}

bool ShapeMatcher::readShapeModel(const std::string& filename)
{
    std::ifstream f(filename.c_str(), std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamoff sz = f.tellg();
    if (sz <= 0) return false;
    f.seekg(0, std::ios::beg);

    std::vector<uchar> buf((size_t)sz);
    f.read((char*)&buf[0], sz);
    if (!f) return false;

    return deserializeShapeModel(buf);
}
