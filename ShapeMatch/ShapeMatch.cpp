//***********************************************************************
//  Halcon-like shape based template matching (edge/gradient based)
//***********************************************************************
#include "ShapeMatch/ShapeMatch.h"

#include <opencv2/core/ocl.hpp>
#include <algorithm>
#include <cmath>
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
#  if defined(__AVX2__) || defined(_M_X64)
#    if !defined(_M_IX86)   // 32-bit MSVC cannot reach ymm without /arch
#      define SM_USE_AVX2 1
#      include <immintrin.h>
#    else
#      define SM_USE_AVX2 0
#    endif
#  else
#    define SM_USE_AVX2 0
#  endif
#endif
#if SM_USE_AVX2
#  include <immintrin.h>
#endif

// restrict qualifier: MSVC spells it __restrict, gcc/clang __restrict__
#if !defined(SM_RESTRICT)
#  if defined(_MSC_VER)
#    define SM_RESTRICT __restrict
#  else
#    define SM_RESTRICT __restrict__
#  endif
#endif

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

struct XForm
{
    std::vector<int16_t> ox, oy;
    std::vector<float>   nx, ny;
    int  R = 0;
    int  n = 0;
    cv::Mat kx, ky;
    bool hasKernel = false;
};

namespace
{
    inline uint64_t makeKey(int level, int aIdx, int sIdx)
    {
        return (uint64_t)(level) << 44 | (uint64_t)(aIdx & 0xFFFFF) << 20 | (uint64_t)(sIdx & 0xFFFFF);
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
    int minSide = 24;
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
    void growEdges(const cv::Mat& mag, const cv::Mat& thin, float lo, float hi, cv::Mat& edge)
    {
        const int w = mag.cols, h = mag.rows;
        edge.create(h, w, CV_8U);
        edge.setTo(0);
        const float* pm = mag.ptr<float>();
        const uchar* pt = thin.ptr<uchar>();

        uchar* pe = edge.ptr<uchar>();
        std::vector<int> stack;
        for (int i = 0; i < w * h; i++)
            if (pt[i] && pm[i] >= hi && !pe[i])
            {
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
                    if (er[xx] || !tr[xx] || mr[xx] < lo) continue;
                    er[xx] = 1;
                    stack.push_back(yy * w + xx);
                }
            }
        }
    }

    // walk the thinned edges into chains and take the points at equal arc
    // length, so the model follows the texture instead of clustering on the
    // few strongest ridges (this is what Halcon's model contours look like)
    std::vector<int> sampleAlongContours(const cv::Mat& edge, const cv::Mat& mag, int maxPts, int minChainLen)
    {
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
        if (chains.empty() || maxPts <= 0) return out;

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

        if ((int)chains.size() > maxPts)
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

        out.reserve((size_t)maxPts);
        for (size_t k = 0; k < chains.size(); k++)
        {
            const std::vector<int>& ch = chains[k];
            for (int j = 0; j < quota[k]; j++)
            {
                const size_t t = (size_t)((double)(j + 0.5) * ch.size() / quota[k]);
                out.push_back(ch[std::min(t, ch.size() - 1)]);
            }
        }
        // strongest first: the scan loop prunes on the first points it sums up
        std::sort(out.begin(), out.end(), [pm](int a, int b) { return pm[a] > pm[b]; });
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

    // the number of model points is kept roughly constant over the levels
    int maxPts = params_.maxPoints;
    if (level > 0)
        maxPts = std::max(64, params_.maxPoints / (1 << level));

    // origin = centre of the model image, mapped to this pyramid level and
    // snapped to a full pixel (otherwise the rounding of the offsets and the
    // rounding of the match position would add up to a one pixel error)
    const int icx = (modelGray0_.cols / 2) >> level;
    const int icy = (modelGray0_.rows / 2) >> level;
    ld.center = cv::Point2f((float)icx, (float)icy);
    ld.points.clear();
    ld.activePoints = 0;
    if (level == 0) { candCount_ = 0; strongestMag_ = 0; weakestMag_ = 0; }

    const float lo = (float)std::max(params_.minContrast, 1.0);
    const float hi = (float)std::max(params_.contrast, lo * 1.5);
    thinRidges(mag, gx, gy, thin);
    growEdges(mag, thin, lo, hi, edge);

    // mask off edges outside the region of interest
    if (!mask.empty())
        cv::bitwise_and(edge, mask, edge);

    const std::vector<int> idx = sampleAlongContours(edge, mag, maxPts, params_.minChainLen);
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

    if (level == 0 && n > 0)
    {
        candCount_    = cv::countNonZero(edge);
        strongestMag_ = pMag[idx.front()];
        weakestMag_   = pMag[idx.back()];
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
    xf.nx.resize(n); xf.ny.resize(n);
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
        xf.nx[k] = vx;
        xf.ny[k] = vy;
        int m = std::max(std::abs(ix), std::abs(iy));
        if (m > R) R = m;
    }
    xf.R = R;
    xf.n = n;
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
        T gx, gy, mag, den, msk, tx, ty;
        cv::Sobel(gray, gx, CV_32F, 1, 0, 3);
        cv::Sobel(gray, gy, CV_32F, 0, 1, 3);
        cv::magnitude(gx, gy, mag);
        cv::max(mag, cv::Scalar(minContrast), den);
        cv::divide(gx, den, tx);
        cv::divide(gy, den, ty);
        cv::threshold(mag, msk, minContrast, 1.0, cv::THRESH_BINARY);
        cv::multiply(tx, msk, sx);
        cv::multiply(ty, msk, sy);
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

        ScanInvoker(const cv::Mat& _sx, const cv::Mat& _sy, const XForm& _xf,
                    cv::Mat& _out, float _target, float _slope)
            : sx(&_sx), sy(&_sy), xf(&_xf), out(&_out), target(_target), slope(_slope) {}

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
                // 16 positions per iteration: two 256-bit float accumulators.
                // Each gather then touches 16 halves = 32 B, twice as much of
                // the 64 B cache line as the 8-wide variant.
                for (; x + 16 <= xEnd; x += 16)
                {
                    __m256 sum0 = _mm256_setzero_ps();
                    __m256 sum1 = _mm256_setzero_ps();
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
                            sum0 = _mm256_add_ps(sum0, _mm256_add_ps(_mm256_mul_ps(na, a0),
                                                                     _mm256_mul_ps(nb, b0)));
                        }
                        if (!dead1)
                        {
                            __m256 a1 = _mm256_cvtph_ps(_mm256_extracti128_si256(ha, 1));
                            __m256 b1 = _mm256_cvtph_ps(_mm256_extracti128_si256(hb, 1));
                            sum1 = _mm256_add_ps(sum1, _mm256_add_ps(_mm256_mul_ps(na, a1),
                                                                     _mm256_mul_ps(nb, b1)));
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
                    _mm256_storeu_ps(orow + x - R,
                                     dead0 ? vDead : _mm256_mul_ps(sum0, vInvN));
                    _mm256_storeu_ps(orow + x - R + 8,
                                     dead1 ? vDead : _mm256_mul_ps(sum1, vInvN));
                }
                // eight neighbouring positions share the same offsets, so the
                // eight source values are consecutive in memory -> one load
                for (; x + 8 <= xEnd; x += 8)
                {
                    __m256 sum = _mm256_setzero_ps();
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
                        sum = _mm256_add_ps(sum, _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(pnx[k]), a),
                                                              _mm256_mul_ps(_mm256_set1_ps(pny[k]), b)));
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
                        _mm256_storeu_ps(orow + x - R, _mm256_mul_ps(sum, vInvN));
                }
#endif
#if SM_USE_SSE2
                // four neighbouring positions share the same offsets, so the
                // four source values are consecutive in memory -> one vector load
                const __m128 vInvN128 = _mm_set1_ps(invN);
                for (; x + 4 <= xEnd; x += 4)
                {
                    __m128 sum = _mm_setzero_ps();
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
                        sum = _mm_add_ps(sum, _mm_add_ps(_mm_mul_ps(_mm_set1_ps(pnx[k]), a),
                                                         _mm_mul_ps(_mm_set1_ps(pny[k]), b)));
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
                        __m128 v = _mm_set1_ps(-1.f);
                        _mm_storeu_ps(orow + x - R, v);
                    }
                    else
                        _mm_storeu_ps(orow + x - R, _mm_mul_ps(sum, vInvN128));
                }
#endif
                for (; x < xEnd; x++)
                {
                    float sum = 0.f;
                    bool dead = false;
                    for (int k = 0; k < n; k++)
                    {
                        int ix = x + pox[k];
                        int ri = poy[k] + R;
                        sum += pnx[k] * f16ToF32(rx[ri][ix]) + pny[k] * f16ToF32(ry[ri][ix]);
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
                    orow[x - R] = dead ? -1.f : sum * invN;
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
    scoreMap.create(srcGx_[level].rows - K + 1, srcGx_[level].cols - K + 1, CV_32F);

    double g = std::min(std::max(greed, 0.0), 0.98);
    float slope = (float)((1.0 - g * target) / ((1.0 - g) * std::max(1, xf.n)));

    ScanInvoker inv(srcGx_[level], srcGy_[level], xf, scoreMap, (float)target, slope);
    // when we are already inside a parallel loop the rows must be scanned
    // serially: nested parallel_for_ corrupts the OpenCV thread pool
    double stripes = nested ? 1.0 : (double)std::max(1, scoreMap.rows / 4);
    cv::parallel_for_(cv::Range(0, scoreMap.rows), inv, stripes);
}

//***********************************************************************
//  single position scoring (sparse, used while refining)
//***********************************************************************
namespace
{
    inline float scorePos(const cv::Mat& sx, const cv::Mat& sy, const XForm& xf,
                          int ay, int ax, float limit)
    {
        const int n = xf.n;
        const int16_t* pox = xf.ox.data();
        const int16_t* poy = xf.oy.data();
        const float* pnx = xf.nx.data();
        const float* pny = xf.ny.data();
        float sum = 0.f;
        const float lim = limit * n;
        for (int k = 0; k < n; k++)
        {
            int yy = ay + poy[k];
            int xx = ax + pox[k];
            const uint16_t* rx = sx.ptr<uint16_t>(yy);
            const uint16_t* ry = sy.ptr<uint16_t>(yy);
            sum += pnx[k] * f16ToF32(rx[xx]) + pny[k] * f16ToF32(ry[xx]);
            if (limit > -9.f && k > (n >> 2))
            {
                // every term is at most 1 -> stop when the target can no longer be reached
                if (sum + (float)(n - k - 1) < lim)
                    return -1.f;
            }
        }
        return sum / n;
    }
}

//=======================================================================
//  refine all candidates one level down
//=======================================================================
void ShapeMatcher::refineLevel(int level, std::vector<Candidate>& cands,
                               int maxKeep, double minScore)
{
    const int stride = 1 << level;
    const int nA = (int)angleGrid_.size();
    const int nS = (int)scaleGrid_.size();
    // the parent position carries an error of up to one parent pixel, which is
    // two pixels of this level -> the window has to cover at least +/-2
    const int win = 2;
    const int W = 2 * win + 1;

    std::vector<Candidate> refined;
    refined.reserve(cands.size());

    for (size_t ci = 0; ci < cands.size(); ci++)
    {
        const Candidate sc = cands[ci];
        int as[5], ss[5];
        int na = 0, ns = 0;
        for (int k = -2; k <= 2; k++)
        {
            int a = sc.angleIdx + k * stride;
            if (a >= 0 && a < nA) as[na++] = a;
            int s = sc.scaleIdx + k * stride;
            if (s >= 0 && s < nS) ss[ns++] = s;
        }
        if (na == 0) as[na++] = sc.angleIdx;
        if (ns == 0) ss[ns++] = sc.scaleIdx;
        if (na == 0 || ns == 0) continue;

        Candidate best;
        best.score = -2.f;
        best.level = level;
        std::vector<float> scoreBuf((size_t)na * ns * W * W, -2.f);
        int bi = 0, bj = 0, bdx = win, bdy = win;

        for (int i = 0; i < na; i++)
        {
            for (int j = 0; j < ns; j++)
            {
                XForm& xf = getXForm(level, as[i], ss[j]);
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
                                      : scorePos(srcGx_[level], srcGy_[level], xf, by, bx, limit);
                        scoreBuf[(((size_t)i * ns + j) * W + (dy + win)) * W + (dx + win)] = v;
                        if (v > best.score)
                        {
                            best.score = v;
                            best.level = level;
                            best.pos = cv::Point2f((float)bx, (float)by);
                            best.angleIdx = as[i];
                            best.scaleIdx = ss[j];
                            best.angle = (float)angleGrid_[as[i]];
                            best.scale = (float)scaleGrid_[ss[j]];
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
            refined.push_back(best);
        }
    }

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
                std::abs(c.angle - o.angle) <= 1e-4f + (float)levelAngleStep_[level] &&
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
void ShapeMatcher::subpixelRefine(int level, Candidate& c)
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

    float s0 = scorePos(srcGx_[level], srcGy_[level], xf0, ay, ax, -10.f);
    float spx = scorePos(srcGx_[level], srcGy_[level], xf0, ay, ax + 1, -10.f);
    float smx = scorePos(srcGx_[level], srcGy_[level], xf0, ay, ax - 1, -10.f);
    float spy = scorePos(srcGx_[level], srcGy_[level], xf0, ay + 1, ax, -10.f);
    float smy = scorePos(srcGx_[level], srcGy_[level], xf0, ay - 1, ax, -10.f);

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
        float vp = scorePos(srcGx_[level], srcGy_[level],
                            getXForm(level, ap, c.scaleIdx), ay, ax, -10.f);
        float vm = scorePos(srcGx_[level], srcGy_[level],
                            getXForm(level, am, c.scaleIdx), ay, ax, -10.f);
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
        float vp = scorePos(srcGx_[level], srcGy_[level],
                            getXForm(level, c.angleIdx, sp), ay, ax, -10.f);
        float vm = scorePos(srcGx_[level], srcGy_[level],
                            getXForm(level, c.angleIdx, sm), ay, ax, -10.f);
        float d = 2.f * v0 - vp - vm;
        if (d > 1e-6f)
        {
            float t = 0.5f * (vp - vm) / d;
            t = std::min(std::max(t, -1.f), 1.f);
            c.scale = (float)(scaleGrid_[c.scaleIdx] + t * scaleStep_);
        }
    }
}

//=======================================================================
//  local maxima of a score map
//=======================================================================
namespace
{
    // 3x3 maximum, same result as cv::dilate(m, cv::Mat()) but completely
    // serial.  cv::dilate runs its own parallel_for_, and this function is
    // called from inside a parallel loop - nesting them corrupts the OpenCV
    // thread pool and crashes.  Out of range neighbours are clamped, which
    // only ever duplicates a value that is already part of the maximum, so
    // the result matches cv::dilate's BORDER_CONSTANT behaviour.
    void dilate3x3Serial(const cv::Mat& src, cv::Mat& dst)
    {
        CV_Assert(src.type() == CV_32F);
        dst.create(src.size(), CV_32F);
        const int h = src.rows, w = src.cols;
        for (int y = 0; y < h; y++)
        {
            const float* pm = src.ptr<float>(std::max(0, y - 1));
            const float* pc = src.ptr<float>(y);
            const float* pp = src.ptr<float>(std::min(h - 1, y + 1));
            float* o = dst.ptr<float>(y);
            for (int x = 0; x < w; x++)
            {
                const int xm = std::max(0, x - 1);
                const int xp = std::min(w - 1, x + 1);
                float m = pc[x];
                m = std::max(m, pc[xm]); m = std::max(m, pc[xp]);
                m = std::max(m, pm[xm]); m = std::max(m, pm[x]);
                m = std::max(m, pm[xp]);
                m = std::max(m, pp[xm]); m = std::max(m, pp[x]);
                m = std::max(m, pp[xp]);
                o[x] = m;
            }
        }
    }

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
    cv::Mat dil;
    dilate3x3Serial(scoreMap, dil);

    std::vector<Candidate> local;
    const int h = scoreMap.rows, w = scoreMap.cols;
    for (int y = 0; y < h; y++)
    {
        const float* ps = scoreMap.ptr<float>(y);
        const float* pd = dil.ptr<float>(y);
        for (int x = 0; x < w; x++)
        {
            float v = ps[x];
            if (v < (float)thresh) continue;
            if (v < pd[x] - 1e-5f) continue;

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
    gray.convertTo(srcPyramid_[0], CV_8U);
    for (int i = 1; i < levels; i++)
        cv::pyrDown(srcPyramid_[i - 1], srcPyramid_[i]);

    srcGx_.resize(levels);
    srcGy_.resize(levels);
    uGx_.resize(levels);
    uGy_.resize(levels);

    const double mc = params_.minContrast;

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
        }
    }
    else
    {
        for (int i = 0; i < levels; i++)
        {
            cv::Mat g8;
            srcPyramid_[i].convertTo(g8, CV_8U);
            cv::Mat gx32, gy32;
            normGradients<cv::Mat>(g8, gx32, gy32, mc);
            // the scan loop is memory bound, so keep the gradients as halves
            gx32.convertTo(srcGx_[i], CV_16F);
            gy32.convertTo(srcGy_[i], CV_16F);
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
    else gray.convertTo(g8, CV_8U);

    const int nLevels = (int)modelLevels_.size();
    useOCL_ = fp.useOpenCL && oclUsable();

    int64 t0 = cv::getTickCount();
    prepareSource(g8, nLevels, useOCL_);
    int64 t1 = cv::getTickCount();
    lastPreprocessTime_ = (double)(t1 - t0) * 1000.0 / cv::getTickFrequency();

    const int top = nLevels - 1;
    const int nA = (int)angleGrid_.size();
    const int nS = (int)scaleGrid_.size();

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

    // at the coarsest level a lower score must be accepted
    const double topTarget = std::max(0.10, fp.minScore * 0.55);

    std::vector<Candidate> cands;
    {
        std::vector<Candidate> all;
        cv::Mat map;
        double maxMap = -2;
        int nDense = 0;

        // all (angle, scale) poses of the coarsest level
        std::vector<std::pair<int, int>> poses;
        for (int si = 0; si < nS; si += strideS)
            for (int ai = 0; ai < nA; ai += strideA)
                poses.emplace_back(ai, si);

        if (useOCL_)
        {
            // the dense path may build a correlation kernel inside the cache,
            // which is not thread safe -> keep this one serial
            for (size_t p = 0; p < poses.size(); p++)
            {
                const int ai = poses[p].first, si = poses[p].second;
                int R = 0;
                const XForm& xf = getXForm(top, ai, si);
                const int K = 2 * xf.R + 1;
                if (K * K <= 6 * std::max(1, xf.n))
                {
                    correlate(top, ai, si, map, R);
                    nDense++;
                }
                else
                    correlateSparse(top, ai, si, map, R, topTarget, fp.greediness);
                if (fp.debug)
                {
                    double mn, mx;
                    cv::minMaxLoc(map, &mn, &mx);
                    if (mx > maxMap) maxMap = mx;
                }
                extractCandidates(map, top, ai, si, R, topTarget, all, 64, 2);
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
                    for (int i = r.start; i < r.end; i++)
                    {
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
                        correlateSparseX(top, *pxf, lmap, R, topTarget,
                                         fp.greediness, true);
                        if (fp.debug)
                        {
                            double mn, mx;
                            minMaxSerial(lmap, mn, mx);
                            if (mx > localMax) localMax = mx;
                        }
                        extractCandidates(lmap, top, ai, si, R, topTarget, local, 64, 2);
                    }
                    cv::AutoLock lock(mtx);
                    all.insert(all.end(), local.begin(), local.end());
                    if (localMax > maxMap) maxMap = localMax;
                },
                std::max(1.0, (double)poses.size() / 8.0));
        }
        if (fp.debug)
        {
            std::cout << "  [dbg] levels=" << nLevels << " top=" << top
                      << " topTarget=" << topTarget
                      << " angles=" << nA << " scales=" << nS
                      << " dense=" << nDense
                      << " mapMax=" << maxMap
                      << " candidates=" << all.size() << "\n";
        }
        std::sort(all.begin(), all.end(),
                  [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        int keep = std::max(fp.maxMatches * 4, 40);
        for (size_t i = 0; i < all.size() && (int)cands.size() < keep; i++)
        {
            const Candidate& c = all[i];
            bool dup = false;
            for (size_t j = 0; j < cands.size(); j++)
            {
                const Candidate& o = cands[j];
                if (std::abs(c.pos.x - o.pos.x) <= 3.f && std::abs(c.pos.y - o.pos.y) <= 3.f &&
                    std::abs(c.angle - o.angle) <= 1e-4f + (float)levelAngleStep_[top] &&
                    std::abs(c.scale - o.scale) <= 1e-4f + (float)levelScaleStep_[top] * 0.5f)
                {
                    dup = true; break;
                }
            }
            if (!dup) cands.push_back(c);
        }
    }

    const int maxKeep = std::max(fp.maxMatches * 3, 30);
    for (int l = top - 1; l >= 0; l--)
    {
        refineLevel(l, cands, maxKeep, fp.minScore);
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
        for (size_t i = 0; i < cands.size(); i++)
            subpixelRefine(0, cands[i]);
    }

    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    const float minD = (float)std::max(1.0, fp.minDistance);
    for (size_t i = 0; i < cands.size() && (int)results.size() < fp.maxMatches; i++)
    {
        const Candidate& c = cands[i];
        if (c.score < fp.minScore) break;
        bool dup = false;
        for (size_t j = 0; j < results.size(); j++)
        {
            const MatchResult& o = results[j];
            if (cv::norm(c.pos - o.pos) < minD &&
                std::abs(c.angle - o.angle) < 0.35 &&
                std::abs(c.scale - o.scale) < 0.12)
            {
                dup = true; break;
            }
        }
        if (!dup)
        {
            MatchResult m;
            m.pos = c.pos;
            m.angle = c.angle;
            m.scale = c.scale;
            m.score = c.score;
            results.push_back(m);
        }
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
    return scorePos(srcGx_[0], srcGy_[0], xf, ay, ax, -10.f);
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
//    params   : MatchParams
//    modelGray0_
//    levels   : uint32 nLevels, then per level
//                 Size | Point2f center | int activePoints | uint32 n
//                 then n x ModelPoint (5 x float32)
//    grids    : angleGrid_ | scaleGrid_ | angleStep_ scaleStep_ scaleMin_
//               levelAngleStep_ levelScaleStep_
//               levelAngleCount_ levelScaleCount_
//=======================================================================
namespace
{
    const uint32_t SM_MAGIC   = 0x314D4853;   // 'SHM1'
    const uint32_t SM_VERSION = 1;

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

    return w.b;
}

bool ShapeMatcher::deserializeShapeModel(const std::vector<uchar>& data)
{
    ByteReader r;
    r.p = data.empty() ? nullptr : &data[0];
    r.n = data.size();

    uint32_t magic = 0, version = 0;
    r.u32(magic); r.u32(version);
    if (magic != SM_MAGIC || version != SM_VERSION)
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
