//***********************************************************************
//  Halcon-like NCC (normalised cross correlation) template matching.
//
//  The parameter set and the reported result follow Halcon's NCC operators:
//      createNccModel  ==  create_ncc_model
//      findNccModel    ==  find_ncc_model
//      writeNccModel   ==  write_ncc_model
//      readNccModel    ==  read_ncc_model
//
//  Differences against the shape matcher in ShapeMatch/:
//    * shape match compares GRADIENT DIRECTIONS (sign + magnitude free)
//    * NCC compares GRAY VALUES (normalised, so brightness/contrast free)
//  NCC therefore wins on low-contrast / blurred / pure-texture surfaces and on
//  "is this the same surface" questions, and loses on repetitive patterns
//  (a repeated texture correlates just as well everywhere).
//
//  What is NOT implemented (Halcon has it, NCC cannot do it):
//    * scale search.  Halcon's NCC is rotation only - there is no
//      ScaleMin/ScaleMax in create_ncc_model and no Scale output in
//      find_ncc_model.  Same here.
//
//  Threading: createNccModel / readNccModel are not thread safe on the same
//  object (they mutate it).  findNccModel() is const and only reads, so once a
//  model is built the same object may be searched from several threads, and
//  several NccMatcher objects may be searched in parallel.
//***********************************************************************
#pragma once
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

#if defined(GEOMATCH_EXPORTS)
#  if defined(_WIN32) || defined(_WIN64)
#    define NCCMATCH_API __declspec(dllexport)
#  else
#    define NCCMATCH_API __attribute__((visibility("default")))
#  endif
#elif defined(GEOMATCH_SHARED)
#  if defined(_WIN32) || defined(_WIN64)
#    define NCCMATCH_API __declspec(dllimport)
#  else
#    define NCCMATCH_API
#  endif
#else
#  define NCCMATCH_API
#endif

namespace geomatch
{


//-----------------------------------------------------------------------
//  create_ncc_model parameters
//-----------------------------------------------------------------------
struct NccParams
{
    int    numLevels   = 0;             // 0 = auto (same rule as the shape matcher)
    double angleStart  = -CV_PI;        // radians
    double angleExtent = 2 * CV_PI;     // radians
    double angleStep   = 0.0;           // 0 = auto (1 degree at level 0)
    // Halcon's Metric; NCC only has these two:
    //   0 = 'use_polarity'           the template's bright/dark must match
    //   1 = 'ignore_global_polarity' a fully inverted template still matches,
    //                                i.e. score = |NCC|
    // (NCC has no 'ignore_local_polarity'; that only exists for shape models.)
    int    metric      = 0;
    // Optional region of interest inside the template.  Empty = whole rectangle.
    // The score is always evaluated over the rotated template's full bounding
    // box; the mask only removes pixels from the *template* side, exactly like
    // passing a reduced domain to create_ncc_model.
    cv::Mat mask;                       // CV_8U, non-zero = use this pixel
};

//-----------------------------------------------------------------------
//  find_ncc_model parameters
//-----------------------------------------------------------------------
struct NccFindParams
{
    double minScore   = 0.7;   // NCC 分数范围 [-1,1]，0.7 算"较像"，0.8 以上基本确定
    int    maxMatches = 1;
    // Halcon's MaxOverlap: two reported instances may not overlap by more than
    // this fraction of the smaller rotated rectangle.  0.5 is Halcon's default
    // for NCC.
    double maxOverlap = 0.5;
    // Halcon's SubPixel: 0 = 'none', 1 = 'interpolation' (parabola on the score
    // surface, position and angle).  'least_squares' is not implemented.
    int    subpixel   = 1;
    // Halcon's NumLevels in find_ncc_model: coarsest level to start from,
    // 0 = use every level the model has.
    int    numLevels  = 0;
    int    debug      = 0;
};

//-----------------------------------------------------------------------
//  one found instance.  Halcon returns Row / Column / Angle / Score; there is
//  no Scale because NCC does not search scale.
//-----------------------------------------------------------------------
struct NccResult
{
    cv::Point2f pos;            // model centre in the search image
    double      angle = 0.0;    // radians
    double      score = 0.0;    // NCC in [-1, 1]; with metric 1 it is |NCC|
};


//-----------------------------------------------------------------------
//  the matcher
//-----------------------------------------------------------------------
class NCCMATCH_API NccMatcher
{
public:
    NccMatcher();
    ~NccMatcher();

    //---- model ---------------------------------------------------------
    bool createNccModel(const cv::Mat& gray, const NccParams& p);
    bool writeNccModel(const std::string& file) const;
    bool readNccModel(const std::string& file);
    bool isCreated() const { return created_; }

    // same, but from / into memory
    std::vector<uchar> serializeNccModel() const;
    bool deserializeNccModel(const std::vector<uchar>& data);

    //---- searching -----------------------------------------------------
    std::vector<NccResult> findNccModel(const cv::Mat& gray, const NccFindParams& p) const;

    // score at one exact pose (tuning / verification, no search)
    double probe(const cv::Mat& gray, cv::Point2f pos, double angle) const;

    //---- pose & geometry (host / robot integration) ----------------------
    // Same set as geomatch::Matcher, so the two matchers are used the same
    // way.  NCC has no scale dimension, so the pose is a rotation plus a
    // translation:
    //     imagePt = pos + R(angle) * (tplPt - origin())
    //     R(a)    = [cos a, sin a; -sin a, cos a]
    // The reported NccResult::pos is the reference point, the template centre
    // by default.
    //
    //   poseMatrix()  [vector_angle_to_rigid]  template -> image, 3x3
    //   poseAffine()  the same as the 2x3 form cv::warpAffine wants
    //   cornersAt()   the 4 template corners at this pose
    //   boundsAt()    axis-aligned box around cornersAt()
    //   templateToImage() / imageToTemplate()   one point, exact inverse
    //   setOrigin()   [set_ncc_model_origin]   move the reported point
    cv::Matx33d poseMatrix(const NccResult& r) const;
    cv::Mat     poseAffine(const NccResult& r) const;
    std::vector<cv::Point2f> cornersAt(const NccResult& r) const;
    cv::Rect2f  boundsAt(const NccResult& r) const;
    cv::Point2f templateToImage(const NccResult& r, cv::Point2f tplPt) const;
    cv::Point2f imageToTemplate(const NccResult& r, cv::Point2f imgPt) const;

    // reference point in TEMPLATE pixel coordinates (Halcon:
    // set_ncc_model_origin).  Not stored in the .ncm - set it after load().
    void        setOrigin(cv::Point2f tplPoint);
    void        resetOrigin();
    cv::Point2f origin() const;

    //---- model facts ---------------------------------------------------
    cv::Mat     modelGray()   const;
    cv::Size    modelSize()   const;      // template size at level 0
    cv::Point2f modelCenter() const;
    int         numLevels()   const;
    int         modelPixelCount() const;  // pixels used at level 0
    double      angleStep()   const;      // radians at level 0
    int         angleCount()  const;
    const NccParams& params() const;

    //---- visualisation -------------------------------------------------
    cv::Mat drawModelFeatures(double scaleFactor = 3.0) const;   // template + mask outline
    cv::Mat drawResult(const cv::Mat& gray, const std::vector<NccResult>& res) const;

    double lastPreprocessTime() const;
    double lastSearchTime() const;

private:
    // reference point of a result, in template pixel coordinates:
    // the value given to setOrigin(), or the template centre otherwise.
    cv::Point2f refPoint() const;

    struct Impl;
    Impl* impl_ = nullptr;
    bool  created_ = false;
    NccParams params_;
    cv::Point2f origin_;                    // setOrigin() value, template coords
    bool        hasOrigin_ = false;
};

} // namespace geomatch
