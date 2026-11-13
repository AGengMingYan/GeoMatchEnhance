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
    double minScore   = 0.5;
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
    struct Impl;
    Impl* impl_ = nullptr;
    bool  created_ = false;
    NccParams params_;
};

} // namespace geomatch
