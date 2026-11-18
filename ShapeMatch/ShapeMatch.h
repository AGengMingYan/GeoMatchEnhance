//***********************************************************************
//  Halcon-like shape based template matching (edge/gradient based)
//  createShapeModel == create_shape_model, findShapeModel == find_shape_model
//
//  This is the low level engine.  Most applications should use the facade in
//  GeoMatch/GeoMatch.h instead; the only reason to include this header
//  directly is the escape hatch geomatch::Matcher::core().
//
//  Threading: createShapeModel / readShapeModel mutate the object and are not
//  thread safe.  findShapeModel() is NOT safe to share either, despite the
//  historical comment here: prepareSource() writes the per-frame buffers
//  (srcPyramid_ / srcGx_ / srcGy_ / srcQ8_ / srcM8_) that live on the object,
//  and cv::Mat::create() may reallocate them.  Measured with two threads on one
//  matcher: different images of the same size -> 80% of the searches returned
//  the OTHER image's result; different sizes -> 0xC0000005 crash.  Give every
//  thread its own ShapeMatcher (loading the same .shm is fine); see the
//  SearchContext refactor in docs/SDK.md 8.
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

// Compile-time truth about the SIMD width the shape engine itself was built
// with: 256 = the AVX2 kernels are compiled in, 0 = scalar fallback.  Do not
// guess from __AVX2__ in another translation unit -- a 32-bit build once had
// __AVX2__ defined in the facade while ShapeMatch.cpp silently compiled every
// 256-bit path out, which cost 2x on preprocessing and 8x on the fused pass.
SHAPEMATCH_API int shapeSimdWidth();

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

    // 金字塔最粗一层的最短边下限（像素）：模型会一直 pyrDown，直到下一层的
    // 短边小于它。这是「模型能有多深」的开关，和 FindParams::numLevels
    // （搜索实际用几层）配套使用。
    //   24（默认 = 历史值）：101x102 模板 -> 3 层，顶层 26x26
    //   12                ：101x102 模板 -> 4 层，顶层 12x13
    // 层越深，粗扫的位置数按 4 倍递减。实测 4096x4096 纯平背景、360°：
    //   3 层 53.8 ms / 4 层 12.0 ms（顶层模板都是 26x26，位置数 99.7 万 vs 23.6 万）
    // Halcon 没有暴露这个参数（它用 find_shape_model 的 NumLevels 在搜索时选），
    // 我们两个都开放，因为它同时决定「模型体积」和「能不能从更粗层开始」。
    // 代价：层越粗，弱/小目标越容易被糊掉而漏检 —— 调小必须验召回。
    int    minSide     = 24;

    // Halcon create_shape_model 的 Optimization：'none' / 'auto' /
    // 'point_reduction_low' / 'medium' / 'high'。
    //
    // Halcon 用它控制「点怎么存」，实测在我们的模板上三种模式**点数完全相同**，
    // 所以这里不为它造一条独立的实现，而是映射到点数预算 maxPoints（这是本库
    // 真正的控点旋钮）：
    //   none / auto        -> maxPoints 不变
    //   point_reduction_low     -> maxPoints x 0.75
    //   point_reduction_medium  -> maxPoints x 0.50
    //   point_reduction_high    -> maxPoints x 0.33
    // 目的只有一个：让 Halcon 配方能原样搬过来不报错。要精确控点请直接用
    // maxPoints；空字符串等同 'none'。非法值会被忽略（保留 maxPoints）。
    std::string optimization = "";

    // Halcon Contrast 的**多段幅值区间**语义：[lo1,hi1,lo2,hi2,...]。
    // 空（默认）= 用下面 contrast/minContrast 的单段滞后阈值（历史行为，
    // 种子要求 mag>=contrast、连接要求 mag>=minContrast，无上限）。
    // 非空 = 种子和连接都要求 mag 落在**任意一段** [lo,hi] 内（含上下界），
    // 用来「只要中等强度边、丢掉最强的高光边」这类需求。
    // 每两项一对（lo,hi）；元素个数为奇数时最后一个被忽略。
    std::vector<double> contrastBands;

    double angleStart  = -CV_PI;
    double angleExtent = 2 * CV_PI;
    double angleStep   = 0;    // 0 = auto (radians)
    double scaleMin    = 1.0;
    double scaleMax    = 1.0;
    double scaleStep   = 0;    // 0 = auto
    double contrast    = 30;   // high threshold: a ridge must reach it to become a seed
    double minContrast = 12;   // low threshold: weaker edges are kept while they touch a seed
    int    maxPoints   = 1200; // model points kept at the finest level.
                               // 0 = no budget: keep every hysteresis pixel (the
                               // coarse levels then scale down by 2^level), which
                               // is what Halcon does; a large / dense template
                               // needs it, 1200 only covers ~150x150 px templates.
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
    // 0.7: 默认评分阈值。默认 scoreMode 是 0（mean，Halcon 语义），同一位姿
    // 的分数比旧的 positive-mean 低约 0.10；需要追召回时可手动调低，但要配合
    // minCoverage / scoreMode 抑制误检（见 README 的"误检抑制"章节）。
    double minScore    = 0.7;
    int    maxMatches  = 20;

    // Halcon 的 find_shape_model(..., NumLevels, ...)：搜索最多用模型的多少层，
    // 从最细的一层往粗数。0（默认）= 用模型的全部层，即历史行为。
    //   * 调小 → 从更细的层开始：位置数变多、变慢，但弱/小目标不容易被粗层糊掉
    //     （Halcon 的 4096² 纯平实测 NumLevels 2/3/4/5 = 190.8/19.1/7.68/9.65 ms）
    //   * 调大 → 从更粗的层开始（上限是模型层数），每多一层位置数 ÷4、角度数 ÷2
    // 与 MatchParams::numLevels（建模层数）不同：这个**不需要重建模型**。
    int    numLevels   = 0;

    // ---- Halcon find_shape_model 的搜索期角度/尺度子范围 ----
    // Halcon 在**搜索时**再给一次 AngleStart / AngleExtent / ScaleMin / ScaleMax，
    // 允许是建模范围的子集（find_shape_model / find_scaled_shape_model 的签名）。
    // 我们原来没有这些字段，`-load` + `-angle` 只能告警拒绝、必须重建模型。
    //
    // 语义：
    //   angleExtent <= 0（默认）  -> 用模型自己的整个角度范围（历史行为）
    //   angleExtent  > 0          -> 只搜 [angleStart, angleStart+angleExtent]
    //                                角度按 2*pi 取模，所以 170°..190° 这种跨 ±180
    //                                的区间也能正确表达
    //   scaleMax <= 0（默认）     -> 用模型自己的整个尺度范围
    //   scaleMin/scaleMax > 0     -> 只搜这个尺度区间（会与模型范围取交集）
    //
    // 精度：位姿集合是模型网格的**子集**，不会凭空产生新位姿；但结果列表可能变
    // （范围外若还有近乎等分的角度位姿，NMS 的胜者会换，分数差通常 < 0.01）。
    // 亚像素会以 ±1 个网格步长向内/外微调，所以落在边界附近的位姿可能略出范围。
    double angleStart  = 0.0;
    double angleExtent = -1.0;
    double scaleMin    = 0.0;
    double scaleMax    = 0.0;

    // 超时（毫秒），0 = 不限（默认）。对齐 Halcon 的
    // set_shape_model_param('timeout', ...)：超时后**返回已经找到的结果**
    // （不是报错），并用 lastSearchTimedOut() 告诉调用方结果不完整。
    // 检查点在粗扫的每个位姿和 refine 的每个候选之间。
    int    timeoutMs   = 0;


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

    // 评分稳健性改进已内建，不再提供开关：
    //  - 软门限梯度归一化：弱梯度按 min(mag/minContrast,1) 给部分权重而非归零，
    //    轻微模糊/光照变化下分数不再塌到 0.4~0.7（见 prepareSource）。
    //  - 亚像素梯度采样：精化/复评在模型点真实旋转位置双线性插值，消除整数
    //    取整的 <=0.5px 抖动，降低斜角误差（粗扫仍用整数卷积核保速度）。

    //----
    // skipWeight（2026-11-15 起默认 0 = 按全部模型点平均，即 Halcon 口径）
    //
    // 语义：> 0 时，图像梯度幅值 < skipWeight 的模型点不参与平均，分母只用
    //       "有效点"（keptAll）；= 0 时全部 n 个点都参与（平坦处 dot ≈ 0 直接
    //       拉低分数），与 Halcon 的"分母永远是全部模型点"一致。
    //
    // 为什么默认改成 0（2026-11-15 实测，模板 = case1 蛇形走线的 U 弯 215×164）：
    //   真值位姿            skipWeight=0.9 -> 0.996 ; =0 -> 0.995 ; Halcon 0.9748
    //   错位 86/89/179 px   skipWeight=0.9 -> 0.743/0.774/0.822（三个都过 minScore 0.7）
    //                       skipWeight=0   -> 0.459/0.508/0.546（只有真值过）
    //                       Halcon         -> 0.486/0.485/0.556
    //   原因：> 0 时"只对上模板下半部分"的位姿把没对上的那半从分母里删掉了，
    //   分数被抬到 0.82；Halcon 不删，所以只有 0.49~0.56。
    //   代价实测很小（同一批模型/参数）：
    //     data1 前 40 张  0.9083 -> 0.9083（逐位相同，点全落在强边上）
    //     data2 36 张      0.6746/35 命中 -> 0.6714/36 命中（min 0.54）
    //     case1 7 张       0.799~0.980（原 0.842~0.989，都远高于 0.7）
    //   也就是说：只有"模板有大片平坦区"时两口径才会差 0.27，正好是会产生
    //   "多个高分重叠结果"的那类模板。要追召回（弱对比目标）再把这一项调到
    //   0.9，用之前那套数据（data2 会多 1 张、分数虚高 0.27）自己权衡。
    //
    // 防御：skipWeight > 0 且有效点数 < max(8, n/4) 时该位置直接判 -1（不可靠）。
    float  skipWeight  = 0.f;

    // 0 = off；>0 = 允许的"模板缺失面积比例"上限，超过即拒绝该匹配。
    // 判据：把模板分成 4×4 格，统计每格里"图像梯度足够强"的点占比；占比 < 0.2
    // 的格算"缺失"，缺失格数 / 总格数 = missShare。
    // "足够强"的参考阈值取 skipWeight，skipWeight = 0（默认口径）时回落到 0.9，
    // 所以这个判据在两种口径下都有效（否则 skipWeight=0 时所有点都算"强"，
    // missShare 恒为 0，闸门等于关掉）。
    //   * 模糊/弱对比的真目标：弱点分散，各格占比 0.65~0.86，最多 1~2 格偏低
    //     → missShare ≈ 0.06~0.13（实测 data1 模糊帧）
    //   * "只对上模板一部分"的错位实例：整行整列缺失
    //     → missShare ≈ 0.5（实测模板只贴一半时 8/16 格为空）
    // 默认 0.35：允许最多约 1/3 面积缺失（真实产线里模板边缘被遮挡/出画面也能过），
    // 同时"只对上模板一半"的错位实例（missShare≈0.5）仍会被拒。
    // 实测余量：data1 最低分真目标 0.25、data2 −90° 帧 0.00、模板只贴一半 0.50。
    float  cellMissMax = 0.35f;
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
    // true = 上一次 findShapeModel() 因为 FindParams::timeoutMs 提前结束，
    // 返回的结果是"到目前为止找到的"，不完整。
    bool   lastSearchTimedOut() const { return timedOut_; }

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
    // 8-bit copy of the same normalized gradients, for the coarse scan kernel:
    //   srcQ8_[i]  CV_8SC2, per pixel (qx,qy) = round(grad*127), 0 where the
    //              point may not be scored (skipWeight mask already folded in)
    //   srcM8_[i]  CV_8U,   1 = this pixel may be scored, 0 = skipped
    // The interleaved pair lets one vpmaddwd turn 8 pixel positions into 8
    // integer dots (see ScanInvoker8).  The halves stay the reference for
    // refine / sub-pixel / re-scoring, so only the coarse ranking uses this.
    std::vector<cv::Mat>  srcQ8_, srcM8_;
    std::vector<cv::UMat> uGx_, uGy_;
    bool useOCL_;
    double lastPreprocessTime_, lastSearchTime_;
    bool timedOut_ = false;      // FindParams::timeoutMs 触发的提前结束


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
                     int metric = 0, int64_t deadline = 0, bool* timedOut = nullptr);
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

    //----
    // "skip weak image gradients" weight, copied from FindParams::skipWeight
    // at the start of every findShapeModel call.  0 = off.  All scoring loops
    // (SIMD scan, scorePos / sub-pixel / final detail) read it: a model point
    // whose sampled gradient length (= min(|grad|/minContrast, 1)) is below
    // this threshold is excluded from the mean and the denominator becomes the
    // kept count.  This is what makes the reported score track Halcon's:
    // blur / weak lighting no longer drag one third of the points to cos ~ 0.1.
    float skipWeight_ = 0.9f;
};
