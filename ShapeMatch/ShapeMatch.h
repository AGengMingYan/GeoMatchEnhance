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
    double minScore    = 0.6;
    int    maxMatches  = 20;
    double minDistance = 40;
    double greediness  = 0.9;
    int    subpixel    = 1;
    bool   useOpenCL   = true;
    int    debug       = 0;
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
    cv::Point2f modelCenter() const;   // centre of gravity of the level-0 model points

    // introspection for the demo / visualisation
    cv::Mat                getModelGray() const { return modelGray0_; }
    const std::vector<ModelPoint>& modelPoints(int level = 0) const { return modelLevels_[level].points; }
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
    void refineLevel(int level, std::vector<Candidate>& cands, int maxKeep, double minScore);
    void subpixelRefine(int level, Candidate& c);
};
