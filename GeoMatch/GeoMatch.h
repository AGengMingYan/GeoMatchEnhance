//***********************************************************************
//  GeoMatch - high level facade around the shape matcher.
//
//  This is the only header a caller needs.  Everything else (the matcher
//  core, the drawing helpers, the synthetic tests) lives inside the
//  library and is not exposed.
//
//  Typical use:
//      geomatch::Matcher m;
//      m.load("model.shm");                       // or m.create(tpl, cfg)
//      auto dets = m.detect(img, &tm);
//      cv::imwrite("out.png", m.drawResult(img, dets));
//***********************************************************************
#pragma once

#include <opencv2/core/types.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "ShapeMatch/ShapeMatch.h"

//-----------------------------------------------------------------------
//  version.  Kept in sync with project(GLOB VERSION) in CMakeLists.txt and
//  with the git tag; geoMatchVersion() returns the same string at runtime so
//  an application can log what it actually linked against.
//-----------------------------------------------------------------------
#define GEOMATCH_VERSION_MAJOR 1
#define GEOMATCH_VERSION_MINOR 1
#define GEOMATCH_VERSION_PATCH 1
#define GEOMATCH_VERSION_STRING "1.1.1"


//-----------------------------------------------------------------------
//  export decoration.  GEOMATCH_STATIC is defined when building/using the
//  static library; otherwise the symbols come from the shared library.
//-----------------------------------------------------------------------
//   GEOMATCH_EXPORTS - set while the library itself is compiled
//   GEOMATCH_SHARED  - set for users of the shared build
//   neither          - static build, nothing to decorate
#if defined(GEOMATCH_EXPORTS)
#  if defined(_WIN32) || defined(_WIN64)
#    define GEOMATCH_API __declspec(dllexport)
#  else
#    define GEOMATCH_API __attribute__((visibility("default")))
#  endif
#elif defined(GEOMATCH_SHARED)
#  if defined(_WIN32) || defined(_WIN64)
#    define GEOMATCH_API __declspec(dllimport)
#  else
#    define GEOMATCH_API
#  endif
#else
#  define GEOMATCH_API
#endif

namespace geomatch
{

//-----------------------------------------------------------------------
//  one configuration for both model creation and search
//-----------------------------------------------------------------------
struct Config
{
    // ---- model ----
    double angleStart   = -CV_PI;
    double angleExtent  = 2 * CV_PI;
    double angleStep    = 0;       // 0 = auto (radians), Halcon AngleStep
    double scaleMin     = 1.0;
    double scaleMax     = 1.0;
    double contrast     = 30.0;  // high edge threshold (hysteresis seed), Halcon Contrast
    double minContrast  = 12.0;  // low edge threshold (hysteresis link), Halcon MinContrast
    int    maxPoints    = 1200;   // model points kept at the finest level; 0 = no
                                  // cap (every hysteresis pixel becomes a point),
                                  // which is what Halcon's create_shape_model does
    int    numLevels    = 0;      // 0 = auto
    cv::Mat mask;                 // CV_8U, non-zero = region that may contribute edge points
    int    minChainLen = 2;       // drop edge chains shorter than this (pixels)

    // ---- search ----
    // 0.7: 默认评分阈值。默认 scoreMode 是 0（mean，Halcon 语义），同一位姿的
    // 分数比旧 positive-mean 低约 0.10。追召回可手动调低，但建议配合 minCoverage
    // / scoreMode 抑制误检；Raise it/lower it once you have measured your data.
    double minScore     = 0.7;
    int    maxMatches   = 12;
    double minDistance  = 30.0;
    // Halcon's MaxOverlap: instances whose model rectangles overlap by more
    // than this fraction cannot both be reported.  Scale independent, so it
    // does not have to be retuned when the template size or the scale range
    // changes.  1.0 = off (default), 0.5 is Halcon's default.
    double maxOverlap   = 1.0;
    double greediness   = 0.9;
    // 0 = none, 1 = parabola interpolation (default), 2 = least squares
    int    subpixel     = 1;
    // Halcon's Metric: 0 = use_polarity (default), 1 = ignore_global_polarity,
    // 2 = ignore_local_polarity.  See FindParams::metric.
    int    metric       = 0;
    bool   useOpenCL    = true;
    int    debug        = 0;
    int    scoreMode    = 0;    // final score aggregation:
                                //   0 = mean        (default, Halcon-compatible)
                                //   2 = trimmed-mean(10%)
                                //   1 = positive-mean  -> clipped/coverage, averages
                                //       ONLY over the agreeing points, so a wrong
                                //       position where most points happen to agree
                                //       scores 0.95+ (measured: 0.946 vs Halcon 0.556
                                //       on the same false pose).  Do not default to it.
                                //   3 = clipped-mean
                                // (positive terms only, but n stays in the
                                // denominator -> no inflation on partial matches).
    float  minCoverage  = 0.f;  // 0 = off.  Minimum fraction of the model points
                                // whose gradient agrees at the reported pose.
                                // The cheapest way to reject the look-alike
                                // positions that appear when minScore is lowered
                                // on repetitive patterns.
    // 分数口径（详见 FindParams::skipWeight）。
    // 0（默认）= 按全部模型点平均 = Halcon 口径：模板某部分在图像上是平坦背景时，
    //   那些点的 dot≈0 会如实拉低分数，"只对上模板一部分"的位姿拿不到高分。
    // 0.9 = 跳过"图像梯度 < 0.9"的模型点、分母只用有效点：弱对比/模糊帧的分数
    //   会高 0.1~0.3（追召回用），但只对上一部分模板的位姿也会被抬到 0.8+。
    float  skipWeight   = 0.f;
    // 0 = 关闭；>0 = 允许的"模板缺失面积比例"上限（4x4 格里没梯度的格占比）。
    // 与 skipWeight 无关地生效，"模板有一块根本没对上"的假目标靠它拒掉。
    float  cellMissMax  = 0.35f;

    // 注：软门限梯度归一化与亚像素梯度采样已内建为默认行为（见 ShapeMatch
    // 的 prepareSource / subpixelRefine），不再提供 Config 开关。

    // ---- visualisation ------------------------------------------------
    // how drawResult() marks a found instance
    //   0 = fixed axis-aligned box, size = template size x scale (default).
    //       Always a level rectangle, so a row of matches reads like a grid
    //       and the box does not wobble with the reported angle.
    //   1 = rotated box: same size, tilted by the match angle (shows the pose)
    //   2 = convex hull of the model points (old behaviour: an irregular
    //       polygon that only looks tidy when the model covers 4 corners)
    int    boxMode      = 0;

    // what is drawn inside the box.  Defaults reproduce Halcon's
    // dev_display_shape_matching_results(): the model contour as a polyline,
    // nothing else.  The arrows and the dots are opt-in because a 1 px
    // contour disappears under them.
    bool   showContour  = true;   // model contour polylines at the pose
    bool   showPoints   = false;  // the sampled scoring points (red dots)
    bool   showGradients = false; // gradient arrows (yellow)

    // build the corresponding low level parameter structs
    MatchParams toMatchParams() const;
    FindParams  toFindParams() const;
};

//-----------------------------------------------------------------------
//  one found instance
//-----------------------------------------------------------------------
struct Detection
{
    cv::Point2f pos;
    float       angle = 0.f;   // radians
    float       scale = 1.f;
    float       score = 0.f;
    float       coverage = 0.f; // fraction of model points that agree (0 when not computed)

    Detection() {}
    Detection(const MatchResult& r)
        : pos(r.pos), angle(r.angle), scale(r.scale), score(r.score),
          coverage(r.coverage) {}
};

//-----------------------------------------------------------------------
//  where the time went.  preprocess/search are the recognition time;
//  read/draw are i/o and visualisation and are NOT part of it.
//-----------------------------------------------------------------------
struct Timing
{
    double preprocessMs = 0;
    double searchMs     = 0;
    double readMs       = 0;   // only filled by runBatch()
    double drawMs       = 0;   // only filled by runBatch()

    double recognitionMs() const { return preprocessMs + searchMs; }
};

//-----------------------------------------------------------------------
//  statistics over a set of images
//-----------------------------------------------------------------------
struct Stats
{
    int    n = 0;
    double min = 0, mean = 0, max = 0, stddev = 0, sum = 0;

    static Stats compute(const std::vector<double>& v);
};

struct BatchReport
{
    int   images  = 0;      // images actually processed
    int   matches = 0;      // total instances found
    Stats preprocess, search, total, read, draw;

    // per image, in the order they were processed
    std::vector<std::string> files;
    std::vector<int>         counts;
    std::vector<double>      preTimes, searchTimes;
    std::vector<std::vector<Detection>> detections;
};

// passed to the runBatch callback after every image
struct BatchProgress
{
    std::string file;
    size_t      index = 0, total = 0;
    int         matches = 0;
    double      preprocessMs = 0, searchMs = 0, readMs = 0, drawMs = 0;
    const std::vector<Detection>* detections = nullptr;
};

//-----------------------------------------------------------------------
//  report of the synthetic angle/scale test
//-----------------------------------------------------------------------
struct GenTestReport
{
    struct Case
    {
        double truthAngleDeg = 0, truthScale = 1;
        bool   found = false;
        double angleDeg = 0, scale = 1, score = 0;
        double errAngleDeg = 0, errScale = 0, errPx = 0;
        double ms = 0;
    };

    std::vector<Case> cases;
    int    found = 0, total = 0;
    double meanErrAngleDeg = 0, maxErrAngleDeg = 0;
    double meanErrScale    = 0, maxErrScale    = 0;
    double meanErrPx       = 0, maxErrPx       = 0;
    double meanScore       = 0, minScore       = 0;
    Stats  timing;
};

//-----------------------------------------------------------------------
//  the facade
//-----------------------------------------------------------------------
class GEOMATCH_API Matcher
{
public:
    Matcher();
    ~Matcher();

    //---- model ---------------------------------------------------------
    bool create(const cv::Mat& gray, const Config& cfg);
    bool load(const std::string& file);
    bool save(const std::string& file) const;
    bool ready() const;

    //---- matching ------------------------------------------------------
    // gray may be 8U or colour; timing is optional (may be null)
    std::vector<Detection> detect(const cv::Mat& gray, Timing* timing = nullptr) const;

    // repeat the search n times and keep the timings of the last run
    std::vector<Detection> detectTimed(const cv::Mat& gray, int loops, Timing* timing = nullptr) const;

    //---- visualisation --------------------------------------------------
    // model image (x3) with the selected edge points, gradients and a
    // parameter panel on the right
    cv::Mat drawModelInfo(const std::string& title = "") const;

    // same template drawing as drawModelInfo() but without the parameter
    // panel: only the enlarged model image, the model points (red), their
    // gradient directions (green) and the model centre (blue).
    // scaleFactor <= 0 means "no enlargement" (1:1 pixel size).
    // drawModelFeatures: template with the model drawn on it.
    // The contour polylines are the model (Halcon's get_shape_model_contours);
    // the gradient arrows are an annotation and are off by default because a
    // 1 px contour disappears under a field of 9 px arrows.
    cv::Mat drawModelFeatures(double scaleFactor = 3.0, bool showGradients = false) const;

    // search image with the template texture overlaid at each match pose,
    // the model points, their gradients and the outline
    cv::Mat drawResult(const cv::Mat& gray, const std::vector<Detection>& dets) const;

    std::vector<cv::Point2d> getDrawResult(const std::vector<Detection>& dets);
    //---- batch ----------------------------------------------------------
    // match every image in the list, write the annotated images into outDir
    // and return the statistics.  writeImages=false skips drawing + imwrite
    // (pure throughput test).  onImage, if set, is called after each image
    // so the caller can report progress.
    BatchReport runBatch(const std::vector<std::string>& files,
                         const std::string& outDir,
                         bool writeImages = true,
                         int  loops = 1,
                         const std::function<void(const BatchProgress&)>& onImage = {});

    //---- synthetic tests ------------------------------------------------
    // generates images at known angle/scale and checks what comes back
    GenTestReport runGenTest(const std::string& outDir, int n, const Config& cfg);

    // cut a patch out of the image, model it and look for it again after
    // rotating/scaling the whole image - verifies pose handling
    struct SelfTestResult
    {
        int    variant = 0;
        double truthAngleDeg = 0, truthScale = 1;
        cv::Point2f truthPos, foundPos;
        bool   found = false;
        double foundAngleDeg = 0, foundScale = 1, score = 0, errPx = 0;
        double probeAtTruth = 0;
    };
    std::vector<SelfTestResult> runSelfTest(const cv::Mat& gray, const Config& cfg);

    //---- model facts ----------------------------------------------------
    cv::Mat     modelGray()       const;
    int         modelPointCount() const;
    int         numLevels()       const;
    cv::Point2f modelCenter()     const;
    cv::Size    modelSize()       const;
    Config      config()          const { return cfg_; }

    //---- how the point set was built (level 0), for tuning --------------
    // candidatePixels() = pixels that survived the hysteresis thresholds.
    // When it is far larger than modelPointCount() the thresholds did nothing
    // and maxPoints alone decided the cut (set maxPoints = 0 to keep them all);
    // when it is smaller the thresholds are the active limit, and the two values
    // are equal.  0 means "unknown" (model read from an older file, where the
    // value is not stored).
    int   candidatePixels() const;
    float strongestMag()    const;   // |g| of the strongest kept point
    float weakestMag()      const;   // |g| of the weakest kept point

    // geometric quality of the point set, see ShapeMatcher::pointSpreadRatio().
    // spreadRatio > 2.5 or normalPeakShare > 0.6 means the model is dominated
    // by one direction and may report high scores at wrong positions once the
    // point count collapses (raise maxPoints / lower minContrast).
    float pointSpreadRatio() const;
    float normalPeakShare()  const;

    // replace the search settings of an already loaded model.
    // read a model first, then adjust only what the search needs:
    //     Config c = m.config();  c.minScore = 0.7;  m.setConfig(c);
    void setConfig(const Config& c) { cfg_ = c; }

    //---- escape hatch ---------------------------------------------------
    // the underlying matcher, for callers that need the low level API
    // (probe(), modelPoints(), ...)
    ShapeMatcher&       core()       { return *core_; }
    const ShapeMatcher& core() const { return *core_; }

private:
    std::unique_ptr<ShapeMatcher> core_;
    Config cfg_;
    std::string modelLabel_;   // shown in drawModelInfo()
};

//-----------------------------------------------------------------------
//  free helpers
//-----------------------------------------------------------------------

// "1.1.0" - the version this library was built from (see GEOMATCH_VERSION_STRING)
GEOMATCH_API const char* versionString();

// is this build a DLL (true) or a static library (false)?
GEOMATCH_API bool isSharedBuild();

// collect the images of a directory, sorted, with the given extensions
GEOMATCH_API std::vector<std::string> listImages(
    const std::string& dir,
    const std::vector<std::string>& exts = { ".jpg", ".png", ".bmp" });

// turn OpenCL on/off; returns true when it is actually usable afterwards
GEOMATCH_API bool enableOpenCL(bool on);

// one line describing the OpenCV build, the CPU features and the devices
GEOMATCH_API std::string environmentReport();

// micro benchmark used to compare two machines: core count, memory bandwidth
// and the throughput of the two kernels the matcher lives on (Sobel and the
// half->float unpack).  every number is also measured with 1, 2, 4 and all
// threads so a machine that does not scale with threads shows up at once.
GEOMATCH_API std::string benchmarkReport();

// drop the template itself from the list so it is never matched against itself
GEOMATCH_API std::vector<std::string> excludeFile(
    const std::vector<std::string>& files, const std::string& templatePath);

} // namespace geomatch
