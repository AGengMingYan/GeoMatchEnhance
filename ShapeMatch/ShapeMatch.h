//***********************************************************************
//  Halcon-like shape based template matching (edge/gradient based)
//  createShapeModel == create_shape_model, findShapeModel == find_shape_model
//***********************************************************************
#pragma once
#include <opencv2/opencv.hpp>
#include <vector>
#include <string>

struct XForm;

//-----------------------------------------------------------------------
//  export decoration, same scheme as the GeoMatch facade:
//    GEOMATCH_EXPORTS - set while the library is compiled
//    GEOMATCH_SHARED  - set for users of the shared build
//    neither          - static build
//-----------------------------------------------------------------------
#if defined(GEOMATCH_EXPORTS)
#  if defined(_WIN32) || defined(_WIN64)
#    define SHAPEMATCH_API __declspec(dllexport)
#  else
#    define SHAPEMATCH_API __attribute__((visibility("default")))
#  endif
#elif defined(GEOMATCH_SHARED)
#  if defined(_WIN32) || defined(_WIN64)
#    define SHAPEMATCH_API __declspec(dllimport)
#  else
#    define SHAPEMATCH_API
#  endif
#else
#  define SHAPEMATCH_API
#endif

struct MatchResult
{
    cv::Point2f pos;   // center of the instance in the search image
    float       angle; // radians
    float       scale;
    float       score;
    // fraction of the model points whose gradient agrees with the image
    // (dot product > 0) at the reported pose.  Filled in when scoreMode != 0
    // or minCoverage > 0; 0 otherwise.  See FindParams::minCoverage.
    float       coverage = 0.f;
};

struct ModelPoint
{
    float x, y;   // position relative to the model centre (level image coords)
    float gx, gy; // gradient
    float mag;    // gradient magnitude
};

struct MatchParams
{
    int    numLevels   = 0;    // 0 = auto
    double angleStart  = -CV_PI;
    double angleExtent = 2 * CV_PI;
    double angleStep   = 0;    // 0 = auto (radians)
    double scaleMin    = 1.0;
    double scaleMax    = 1.0;
    double scaleStep   = 0;    // 0 = auto
    double contrast    = 30;   // high threshold: a ridge must reach it to become a seed
    double minContrast = 12;   // low threshold: weaker edges are kept while they touch a seed
    int    maxPoints   = 1200; // model points kept at the finest level
    cv::Mat mask;              // CV_8U, non-zero = region that may contribute edge points.
                               // empty = the whole template is used (default behaviour).
    int    minChainLen = 2;    // drop edge chains shorter than this (pixels); suppresses
                               // isolated speckle edges while keeping long contours.
    double minScore    = 0.6;
    int    maxMatches  = 20;
    double minDistance = 40;
    int    subpixel    = 1;
};

struct FindParams
{
    // 0.45: the default scoreMode is now 0 (mean, Halcon-compatible), whose
    // scores run ~0.10 lower than the old positive-mean for the same pose.
    double minScore    = 0.45;
    int    maxMatches  = 20;
    // 0 = off.  Minimum centre distance between two reported instances, in
    // pixels at the search image scale.  Kept for compatibility; prefer
    // maxOverlap below because it is scale independent.
    double minDistance = 40;

    // Halcon's MaxOverlap: the model rectangles of two reported instances may
    // not overlap by more than this fraction.  Overlap is
    //   intersection area / area of the smaller rectangle,
    // computed from the two rotated rectangles (template size x scale at the
    // found pose), so it does not have to be retuned when the template size
    // or the scale range changes.  1.0 = off (default, keeps old behaviour),
    // 0 = no overlap at all allowed.  Halcon's default is 0.5.
    double maxOverlap  = 1.0;

    double greediness  = 0.9;

    // Sub-pixel refinement (Halcon's SubPixel):
    //   0 = none          - keep the integer grid result
    //   1 = interpolation - parabola through (p-1, p, p+1) in x, y, angle,
    //                       scale.  Cheap, what this library always did.
    //   2 = least_squares - Gauss-Newton on sum(1 - dot)^2 over
    //                       (x, y, angle, scale) with sub-pixel sampling.
    //                       More accurate on soft/low-contrast edges, slower.
    //   (Halcon's 'minimum_distance' is not implemented.)
    int    subpixel    = 1;

    // Gradient polarity (Halcon's Metric):
    //   0 = use_polarity           - the gradient direction must match the
    //                                template exactly (default, Halcon default)
    //   1 = ignore_global_polarity  - a globally inverted target (bright/dark
    //                                swapped) still matches: the score becomes
    //                                |mean| of the dot products
    //   2 = ignore_local_polarity   - the direction is ignored per point:
    //                                score = mean(|dot|).  Handles targets where
    //                                only part of the edges flip (specular
    //                                metal, mixed lighting).  Slower: the scan
    //                                cannot use the SIMD correlation kernel.
    int    metric      = 0;

    bool   useOpenCL   = true;
    int    debug       = 0;
    int    scoreMode   = 0;    // final score aggregation:
                               //   0 = mean        (default, Halcon-compatible)
                               //   2 = trimmed-mean(10%)
                               //   1 = positive-mean  -> clipped/coverage, i.e. it
                               //       averages ONLY over the points that already
                               //       agree, so a wrong position where 90 % of the
                               //       points happen to agree scores 0.95+.  Do NOT
                               //       make this the default again: measured against
                               //       Halcon 17 on a repetitive-texture template, the
                               //       same false position scored 0.946 with mode 1,
                               //       0.567 with mode 0 and 0.556 in Halcon.
                               //   3 = clipped-mean
                               // search/refine always use mean; this only
                               // re-scores the final matches.
    // 0 = off.  Reject a match whose model points are only partly present:
    // coverage = (#points with dot > 0) / n must be >= minCoverage.
    // This is the cheap answer to "lower minScore and every repetitive
    // structure on the part scores high": a wrong position still matches a
    // large part of the model (high mean / high positive-mean) but leaves a
    // big fraction of the points contradicting it, and coverage falls.
    // Costs one extra n-point pass per reported match.
    float  minCoverage = 0.f;
};

class SHAPEMATCH_API ShapeMatcher
{
public:
    ShapeMatcher();
    ~ShapeMatcher();

    bool createShapeModel(const cv::Mat& gray, const MatchParams& p);
    bool isCreated() const { return created_; }

    //---- persistence (Halcon: write_shape_model / read_shape_model) -------
    // The file holds everything the model needs; the runtime XForm cache is
    // rebuilt on load, so loading is much cheaper than createShapeModel.
    bool writeShapeModel(const std::string& filename) const;
    bool readShapeModel(const std::string& filename);

    // same thing into / from memory (useful to embed the model in a binary)
    std::vector<uchar> serializeShapeModel() const;
    bool deserializeShapeModel(const std::vector<uchar>& data);

    std::vector<MatchResult> findShapeModel(const cv::Mat& gray, const FindParams& p);

    // evaluate the model at one exact pose (level 0), used for testing/tuning
    double probe(const cv::Mat& gray, cv::Point2f pos, double angle, double scale);

    void drawContours(cv::Mat& dst, const MatchResult& m, cv::Scalar color, int thickness = 1) const;
    void drawContours(cv::Mat& dst, cv::Scalar color, int thickness = 1) const;
    int  modelPointCount() const;

    //---- how the point set was built (level 0), for tuning ---------------
    // candidatePixels() is the number of pixels that passed minContrast.  If
    // it is far larger than modelPointCount() then minContrast did nothing
    // and maxPoints alone decided the cut.  weakestMag()/strongestMag() are
    // the gradient magnitudes of the last and the first kept point: a small
    // ratio means the tail of the point set is weak, and weak points are the
    // ones blur and noise destroy first -> lower maxPoints.
    int   candidatePixels()  const { return candCount_; }
    float strongestMag()     const { return strongestMag_; }
    float weakestMag()       const { return weakestMag_; }

    //---- geometric quality of the point set (level 0) ---------------------
    // A model can look fine (many points, high score on the template) and still
    // be unable to discriminate position: if the points lie nearly on one line
    // the model can slide along it (aperture problem), and if they are few the
    // mean score is arithmetic over too little evidence.  Both show up when the
    // contrast threshold is pushed until the point count collapses.
    //
    //   pointSpreadRatio()  std along the major axis / std along the minor axis
    //                       of the point positions.  ~1 = a blob (good),
    //                       > 2.5 = closer to a line (weak in one direction)
    //   normalPeakShare()   fraction of points whose gradient direction falls in
    //                       the single most populated 10 degree bin.  High means
    //                       the model is dominated by one edge orientation.
    float pointSpreadRatio() const { return spreadRatio_; }
    float normalPeakShare()  const { return normalPeakShare_; }
    cv::Point2f modelCenter() const;   // centre of gravity of the level-0 model points

    // introspection for the demo / visualisation
    cv::Mat                getModelGray() const { return modelGray0_; }
    const std::vector<ModelPoint>& modelPoints(int level = 0) const { return modelLevels_[level].points; }
    // the level-0 full edge chains (arc-length order, relative to the model
    // centre).  Empty after reading a model written by an older version.
    const std::vector<std::vector<cv::Point>>& modelChains(int level = 0) const { return modelLevels_[level].chains; }
    int  modelActivePoints(int level = 0) const { return modelLevels_[level].activePoints; }
    int  numLevels() const { return (int)modelLevels_.size(); }
    cv::Size modelSize(int level = 0) const { return modelLevels_[level].size; }
    const MatchParams& params() const { return params_; }

    double lastPreprocessTime() const { return lastPreprocessTime_; }
    double lastSearchTime() const { return lastSearchTime_; }
    static bool openCLAvailable();

private:
    struct LevelData
    {
        std::vector<ModelPoint> points;
        cv::Size   size;
        cv::Point2f center;
        int         activePoints = 0;
        // level-0 only: the FULL edge chains (every thinned edge pixel, in
        // arc-length order) relative to center.  These are what get drawn as
        // connected contours, like Halcon's get_shape_model_contours().  They
        // are display data only - scoring always uses points[].
        std::vector<std::vector<cv::Point>> chains;
    };

    struct Candidate
    {
        int         level = 0;
        cv::Point2f pos;
        double      angle = 0, scale = 1, score = 0;
        int         angleIdx = 0, scaleIdx = 0;
    };

    struct Impl;

    bool created_;
    MatchParams params_;
    // level 0 selection facts, see candidatePixels()/weakestMag()
    mutable int   candCount_    = 0;
    mutable float strongestMag_ = 0;
    mutable float weakestMag_   = 0;
    // level 0 geometry, see pointSpreadRatio()/normalPeakShare()
    mutable float spreadRatio_     = 0;
    mutable float normalPeakShare_ = 0;
    cv::Mat     modelGray0_;
    std::vector<LevelData> modelLevels_;
    Impl* impl_;

    std::vector<double> angleGrid_;
    std::vector<double> scaleGrid_;
    double angleStep_ = 0, scaleStep_ = 0, scaleMin_ = 1;
    std::vector<double> levelAngleStep_, levelScaleStep_;
    std::vector<int>    levelAngleCount_, levelScaleCount_;

    std::vector<cv::Mat>  srcPyramid_, srcGx_, srcGy_;
    std::vector<cv::UMat> uGx_, uGy_;
    bool useOCL_;
    double lastPreprocessTime_, lastSearchTime_;

    void buildAngleScaleGrids();
    void selectEdgePoints(LevelData& ld, const cv::Mat& grayLevel, const cv::Mat& mask, int level) const;
    // level-0 diagnostics: point spread along the principal axes + how much the
    // gradient directions concentrate in one bin (see the header)
    void computeGeometryFacts(const LevelData& ld) const;

    void  buildXForm(int level, int aIdx, int sIdx, XForm& xf) const;
    void  buildKernel(XForm& xf) const;

    // XForm access.  getXForm / warmXForm may insert into the cache and are
    // therefore only safe from one thread at a time.  findXForm never writes
    // and is the only one allowed inside a parallel loop.
    XForm& getXForm(int level, int aIdx, int sIdx);
    XForm& warmXForm(int level, int aIdx, int sIdx);          // never evicts
    const XForm* findXForm(int level, int aIdx, int sIdx) const; // read only, may be null
    XForm& getXFormImpl(int level, int aIdx, int sIdx, bool allowEvict);

    void prepareSource(const cv::Mat& gray, int levels, bool useOCL);
    void correlate(int level, int aIdx, int sIdx, cv::Mat& scoreMap, int& R);
    void correlateSparse(int level, int aIdx, int sIdx, cv::Mat& scoreMap, int& R,
                         double target, double greed, bool nested = false);
    // same, but takes the transform directly: no cache lookup, so it is safe
    // to call from inside a parallel loop
    void correlateSparseX(int level, const XForm& xf, cv::Mat& scoreMap, int& R,
                          double target, double greed, bool nested = false);
    void extractCandidates(const cv::Mat& scoreMap, int level, int aIdx, int sIdx, int R,
                           double thresh, std::vector<Candidate>& out, int maxPerMap, int nmsRadius);
    void refineLevel(int level, std::vector<Candidate>& cands, int maxKeep, double minScore,
                     int metric = 0);
    // mode 1 = parabola interpolation (default), mode 2 = least squares
    void subpixelRefine(int level, Candidate& c, int mode, int metric);

    // Gauss-Newton on sum(1 - dot)^2 over (x, y, angle, scale) with bilinear
    // sampling of the gradient field.  Used by subpixel mode 2.
    void leastSquaresRefine(int level, Candidate& c, int metric);

    // One pose scan that respects the polarity (Halcon Metric):
    //   0 use_polarity          -> the SIMD sparse scan
    //   1 ignore_global_polarity-> two scans (normal + negated normals), max
    //   2 ignore_local_polarity -> scalar scan on mean(|dot|)
    void scanPose(int level, const XForm& xf, cv::Mat& scoreMap, int& R,
                  double target, double greed, int metric, bool nested);

    // metric 2 (ignore_local_polarity) needs mean(|dot|), which the SIMD
    // correlation kernel cannot express, so this is a plain scalar scan.
    void scanLocalPolarity(int level, const XForm& xf, cv::Mat& scoreMap, int& R,
                           double target, double greed, bool nested);
};
