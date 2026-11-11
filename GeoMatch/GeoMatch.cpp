//***********************************************************************
//  GeoMatch - implementation of the facade declared in GeoMatch.h
//***********************************************************************
#include <opencv2/core/types.hpp>
#define _CRT_SECURE_NO_WARNINGS

#include "GeoMatch.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

#include <opencv2/core/ocl.hpp>
#include <opencv2/core/utils/filesystem.hpp>

namespace geomatch
{

static double tickMs(int64 t0)
{
    return (double)(cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();
}

//=======================================================================
//  Config
//=======================================================================
MatchParams Config::toMatchParams() const
{
    MatchParams p;
    p.angleStart   = angleStart;
    p.angleExtent  = angleExtent;
    p.angleStep    = angleStep;
    p.scaleMin     = scaleMin;
    p.scaleMax     = scaleMax;
    p.contrast     = contrast;
    p.minContrast  = minContrast;
    p.maxPoints    = maxPoints;
    p.numLevels    = numLevels;
    p.mask         = mask;
    p.minChainLen  = minChainLen;
    return p;
}

FindParams Config::toFindParams() const
{
    FindParams p;
    p.minScore   = minScore;
    p.maxMatches = maxMatches;
    p.minDistance = minDistance;
    p.greediness = greediness;
    p.subpixel   = subpixel;
    p.useOpenCL  = useOpenCL;
    p.debug      = debug;
    p.scoreMode  = scoreMode;
    return p;
}

//=======================================================================
//  Stats
//=======================================================================
Stats Stats::compute(const std::vector<double>& v)
{
    Stats s;
    if (v.empty()) return s;
    s.n = (int)v.size();
    s.min = s.max = v[0];
    s.sum = 0;
    for (double x : v) { s.min = std::min(s.min, x); s.max = std::max(s.max, x); s.sum += x; }
    s.mean = s.sum / v.size();
    double sq = 0;
    for (double x : v) sq += (x - s.mean) * (x - s.mean);
    s.stddev = (v.size() > 1) ? std::sqrt(sq / (v.size() - 1)) : 0.0;
    return s;
}

//=======================================================================
//  Matcher
//=======================================================================
Matcher::Matcher()  : core_(new ShapeMatcher()) {}
Matcher::~Matcher() {}

bool Matcher::ready() const { return core_ && core_->isCreated(); }

cv::Mat     Matcher::modelGray()       const { return ready() ? core_->getModelGray() : cv::Mat(); }
int         Matcher::modelPointCount() const { return ready() ? core_->modelPointCount() : 0; }
int         Matcher::numLevels()       const { return ready() ? core_->numLevels() : 0; }
cv::Point2f Matcher::modelCenter()     const { return ready() ? core_->modelCenter() : cv::Point2f(); }
cv::Size    Matcher::modelSize()       const { return ready() ? core_->modelSize(0) : cv::Size(); }

bool Matcher::create(const cv::Mat& gray, const Config& cfg)
{
    cfg_ = cfg;
    if (gray.empty()) return false;
    cv::Mat g8;
    if (gray.channels() == 3) cv::cvtColor(gray, g8, cv::COLOR_BGR2GRAY);
    else gray.convertTo(g8, CV_8U);
    return core_->createShapeModel(g8, cfg.toMatchParams());
}

bool Matcher::load(const std::string& file)
{
    if (!core_->readShapeModel(file)) return false;
    cfg_ = Config();
    const MatchParams& p = core_->params();
    cfg_.angleStart  = p.angleStart;
    cfg_.angleExtent = p.angleExtent;
    cfg_.scaleMin    = p.scaleMin;
    cfg_.scaleMax    = p.scaleMax;
    cfg_.minContrast = p.minContrast;
    cfg_.maxPoints   = p.maxPoints;
    cfg_.numLevels   = p.numLevels;
    modelLabel_ = file;
    return true;
}

bool Matcher::save(const std::string& file) const
{
    return core_->writeShapeModel(file);
}

std::vector<Detection> Matcher::detect(const cv::Mat& gray, Timing* timing) const
{
    std::vector<Detection> out;
    if (!ready() || gray.empty()) return out;

    std::vector<MatchResult> res = core_->findShapeModel(gray, cfg_.toFindParams());
    if (timing)
    {
        timing->preprocessMs = core_->lastPreprocessTime();
        timing->searchMs     = core_->lastSearchTime();
        timing->readMs = timing->drawMs = 0;
    }
    out.reserve(res.size());
    for (const MatchResult& r : res) out.push_back(Detection(r));
    return out;
}

std::vector<Detection> Matcher::detectTimed(const cv::Mat& gray, int loops, Timing* timing) const
{
    std::vector<Detection> out;
    if (!ready() || gray.empty()) return out;
    loops = std::max(1, loops);

    std::vector<MatchResult> res;
    for (int l = 0; l < loops; l++)
    {
        res = core_->findShapeModel(gray, cfg_.toFindParams());
        if (timing && l == loops - 1)
        {
            timing->preprocessMs = core_->lastPreprocessTime();
            timing->searchMs     = core_->lastSearchTime();
            timing->readMs = timing->drawMs = 0;
        }
    }
    out.reserve(res.size());
    for (const MatchResult& r : res) out.push_back(Detection(r));
    return out;
}

//=======================================================================
//  model visualisation: enlarged template + edge points + info panel
//=======================================================================
cv::Mat Matcher::drawModelInfo(const std::string& title) const
{
    if (!ready()) return cv::Mat();

    const cv::Mat modelG = core_->getModelGray();
    cv::Mat modelColor;
    cv::cvtColor(modelG, modelColor, cv::COLOR_GRAY2BGR);

    const double sf = 3.0;
    cv::resize(modelColor, modelColor, cv::Size(), sf, sf, cv::INTER_NEAREST);

    const auto& pts = core_->modelPoints(0);
    const cv::Point2f ctr = core_->modelCenter();
    for (int k = 0; k < (int)pts.size(); k++)
    {
        const ModelPoint& p = pts[k];
        cv::Point2f pp((ctr.x + p.x) * (float)sf, (ctr.y + p.y) * (float)sf);
        cv::circle(modelColor, pp, 2, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
        float gnorm = std::sqrt(p.gx * p.gx + p.gy * p.gy);
        if (gnorm > 1e-3f)
        {
            cv::Point2f dir(p.gx / gnorm * 9.f, p.gy / gnorm * 9.f);
            cv::line(modelColor, pp, pp + dir, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
        }
    }
    cv::circle(modelColor, cv::Point(cvRound(ctr.x * sf), cvRound(ctr.y * sf)),
               4, cv::Scalar(255, 0, 0), -1, cv::LINE_AA);

    const MatchParams& P = core_->params();
    const int panelW = 380;
    const int lineH = 28, headerH = 30, sectionGap = 14;

    struct Line { std::string s; cv::Scalar c; double scale; int extra; };
    std::vector<Line> lines;
    auto add = [&](const std::string& s, cv::Scalar c = cv::Scalar(220, 220, 220),
                   double sc = 0.5, int extra = 0)
    {
        lines.push_back({ s, c, sc, extra });
    };

    add("Shape Model", cv::Scalar(0, 255, 255), 0.7, sectionGap);
    if (!title.empty())
        add("source   : " + title);
    add("size     : " + std::to_string(modelG.cols) + " x " +
        std::to_string(modelG.rows));
    add("levels   : " + std::to_string(core_->numLevels()));
    add("points   : " + std::to_string(core_->modelPointCount()),
        cv::Scalar(0, 255, 0));
    add("contrast : hi " + std::to_string((int)P.contrast) +
        " / lo " + std::to_string((int)P.minContrast));
    add("Search Space", cv::Scalar(0, 255, 255), 0.7, sectionGap);
    add("angle    : " + std::to_string((int)(P.angleStart * 180 / CV_PI)) + " .. " +
        std::to_string((int)((P.angleStart + P.angleExtent) * 180 / CV_PI)) + " deg");
    add("scale    : " + std::to_string(P.scaleMin) + " .. " +
        std::to_string(P.scaleMax));
    add("minScore : " + std::to_string(cfg_.minScore));
    add("maxMatch : " + std::to_string(cfg_.maxMatches));
    add("Legend", cv::Scalar(0, 255, 255), 0.7, sectionGap);
    add("red dot   : model point");
    add("green ln  : gradient direction");
    add("blue dot  : model centre");

    auto lineSpacing = [&](const Line& ln)
    {
        return std::max(20, (int)(lineH * ln.scale)) + ln.extra;
    };

    int panelH = headerH + 10;
    for (const auto& ln : lines) panelH += lineSpacing(ln);
    int canvasH = std::max(modelColor.rows, panelH);
    int canvasW = modelColor.cols + panelW + 4;

    cv::Mat canvas = cv::Mat::zeros(canvasH, canvasW, CV_8UC3);
    canvas = cv::Scalar(20, 20, 20);
    int my = (canvasH - modelColor.rows) / 2;
    modelColor.copyTo(canvas(cv::Rect(0, my, modelColor.cols, modelColor.rows)));

    cv::rectangle(canvas, cv::Rect(modelColor.cols, 0, panelW + 4, headerH),
                  cv::Scalar(0, 120, 200), -1);
    cv::putText(canvas, "Model Info", cv::Point(modelColor.cols + 10, 20),
                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);

    int y = headerH + 10;
    for (const auto& ln : lines)
    {
        cv::putText(canvas, ln.s, cv::Point(modelColor.cols + 10, y + (int)(16 * ln.scale)),
                    cv::FONT_HERSHEY_SIMPLEX, ln.scale, ln.c, 1, cv::LINE_AA);
        y += lineSpacing(ln);
    }
    return canvas;
}

//=======================================================================
//  model visualisation without the info panel: template + points + dirs
//=======================================================================
cv::Mat Matcher::drawModelFeatures(double scaleFactor) const
{
    if (!ready()) return cv::Mat();

    const double sf = scaleFactor > 0 ? scaleFactor : 1.0;

    const cv::Mat modelG = core_->getModelGray();
    cv::Mat modelColor;
    cv::cvtColor(modelG, modelColor, cv::COLOR_GRAY2BGR);
    if (sf != 1.0)
        cv::resize(modelColor, modelColor, cv::Size(), sf, sf, cv::INTER_NEAREST);

    const std::vector<ModelPoint>& pts = core_->modelPoints(0);
    const cv::Point2f ctr = core_->modelCenter();
    for (size_t k = 0; k < pts.size(); k++)
    {
        const ModelPoint& p = pts[k];
        cv::Point2f pp((ctr.x + p.x) * (float)sf, (ctr.y + p.y) * (float)sf);
        cv::circle(modelColor, pp, 2, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
        float gnorm = std::sqrt(p.gx * p.gx + p.gy * p.gy);
        if (gnorm > 1e-3f)
        {
            cv::Point2f dir(p.gx / gnorm * 9.f, p.gy / gnorm * 9.f);
            cv::line(modelColor, pp, pp + dir, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
        }
    }
    cv::circle(modelColor, cv::Point(cvRound(ctr.x * sf), cvRound(ctr.y * sf)),
               4, cv::Scalar(255, 0, 0), -1, cv::LINE_AA);

    return modelColor;
}

//=======================================================================
//  result visualisation: template texture overlaid at the match pose
//=======================================================================
cv::Mat Matcher::drawResult(const cv::Mat& gray, const std::vector<Detection>& dets) const
{
    if (gray.empty()) return cv::Mat();

    cv::Mat src = gray;
    if (src.channels() == 3) cv::cvtColor(src, src, cv::COLOR_BGR2GRAY);

    cv::Mat vis;
    cv::cvtColor(src, vis, cv::COLOR_GRAY2BGR);

    if (!ready()) return vis;

    cv::Mat tplBGR;
    cv::cvtColor(core_->getModelGray(), tplBGR, cv::COLOR_GRAY2BGR);
    cv::Mat tplMask(core_->getModelGray().size(), CV_8U, cv::Scalar(255));
    const cv::Point2f tplCtr = core_->modelCenter();
    const std::vector<ModelPoint>& modelPts = core_->modelPoints(0);

    for (size_t k = 0; k < dets.size(); k++)
    {
        const Detection& d = dets[k];
        const cv::Scalar col = (k == 0) ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 200, 255);
        const float ca = (float)std::cos(d.angle), sa = (float)std::sin(d.angle);
        const float s  = d.scale;

        //---- 1) overlay the template texture at the match pose ----------
        // affine map:  searchPos = d.pos + s * R * (p - tplCtr)
        const double tx = d.pos.x - s * (ca * tplCtr.x + sa * tplCtr.y);
        const double ty = d.pos.y - s * (-sa * tplCtr.x + ca * tplCtr.y);
        cv::Mat Mwarp = (cv::Mat_<double>(2, 3) <<
            s * ca,  s * sa, tx,
           -s * sa,  s * ca, ty);
        cv::Mat warped, wmask;
        cv::warpAffine(tplBGR, warped, Mwarp, vis.size(),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar::all(0));
        cv::warpAffine(tplMask, wmask, Mwarp, vis.size(),
                       cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
        cv::Mat blended;
        cv::addWeighted(vis, 0.55, warped, 0.45, 0, blended);
        blended.copyTo(vis, wmask);

        //---- 2) model edge points and their gradient arrows -------------
        for (const ModelPoint& mp : modelPts)
        {
            const float x = s * (mp.x * ca + mp.y * sa) + d.pos.x;
            const float y = s * (-mp.x * sa + mp.y * ca) + d.pos.y;
            cv::circle(vis, cv::Point(cvRound(x), cvRound(y)), 1,
                       cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
            const float gx = mp.gx * ca + mp.gy * sa;
            const float gy = -mp.gx * sa + mp.gy * ca;
            const float gn = std::sqrt(gx * gx + gy * gy);
            if (gn > 1e-3f)
            {
                const cv::Point2f dir(gx / gn * 5.f, gy / gn * 5.f);
                cv::line(vis, cv::Point2f(x, y), cv::Point2f(x, y) + dir,
                         cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
            }
        }

        //---- 3) outline -------------------------------------------------
        MatchResult mr;
        mr.pos = d.pos; mr.angle = d.angle; mr.scale = d.scale; mr.score = d.score;
        core_->drawContours(vis, mr, col, 2);

        //---- 4) caption below the matched box ---------------------------
        std::ostringstream cap;
        cap << "#" << k << "  s=" << std::fixed << std::setprecision(3) << d.score
            << "  a=" << std::setprecision(1) << (d.angle * 180.0 / CV_PI) << "deg"
            << "  k=" << std::setprecision(3) << d.scale;
        std::string txt = cap.str();
        cv::Size ts = cv::getTextSize(txt, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, nullptr);
        int torgX = (int)(d.pos.x - tplCtr.x * s);
        int torgY = (int)(d.pos.y + tplCtr.y * s) + ts.height + 8;
        torgX = std::min(std::max(torgX, 2), std::max(2, vis.cols - ts.width - 4));
        torgY = std::min(std::max(torgY, ts.height + 4), vis.rows - 4);
        cv::Point torg(torgX, torgY);
        cv::rectangle(vis, cv::Rect(torg.x - 2, torg.y - ts.height - 2,
                                    ts.width + 4, ts.height + 4),
                      cv::Scalar(0, 0, 0), -1);
        cv::putText(vis, txt, torg, cv::FONT_HERSHEY_SIMPLEX, 0.5, col, 1, cv::LINE_AA);
    }

    std::ostringstream hdr;
    hdr << "matches=" << dets.size()
        << "   |  overlay: template texture"
        << "   red = model point   yellow = gradient   green = outline";
    cv::rectangle(vis, cv::Rect(0, 0, vis.cols, 24), cv::Scalar(0, 0, 0), -1);
    cv::putText(vis, hdr.str(), cv::Point(6, 17),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
    return vis;
}

std::vector<cv::Point2d> Matcher::getDrawResult(const std::vector<Detection>& dets)
{
    std::vector<cv::Point2d> res;
    const std::vector<ModelPoint>& modelPts = core_->modelPoints(0);
    const cv::Point2f tplCtr = core_->modelCenter();
    for (size_t k = 0; k < dets.size(); k++)
    {
        const Detection& d = dets[k];
        const float ca = (float)std::cos(d.angle), sa = (float)std::sin(d.angle);
        const float s  = d.scale;
        for (const ModelPoint& mp : modelPts) {
            const float x = s * (mp.x * ca + mp.y * sa) + d.pos.x;
            const float y = s * (-mp.x * sa + mp.y * ca) + d.pos.y;
            res.push_back(cv::Point2d(x, y));
        }
    }

    return res;
}

//=======================================================================
//  batch: match a list of images, optionally writing the annotated ones
//=======================================================================
BatchReport Matcher::runBatch(const std::vector<std::string>& files,
                              const std::string& outDir,
                              bool writeImages, int loops,
                              const std::function<void(const BatchProgress&)>& onImage)
{
    BatchReport rep;
    if (!ready()) return rep;

    if (writeImages && !outDir.empty())
        cv::utils::fs::createDirectory(outDir);

    loops = std::max(1, loops);

    std::vector<double> readTimes, drawTimes;

    for (size_t i = 0; i < files.size(); i++)
    {
        int64 tRd0 = cv::getTickCount();
        cv::Mat img = cv::imread(files[i], cv::IMREAD_GRAYSCALE);
        double readMs = tickMs(tRd0);
        if (img.empty()) continue;

        Timing tm;
        std::vector<Detection> dets = detectTimed(img, loops, &tm);

        rep.images++;
        rep.matches += (int)dets.size();
        rep.files.push_back(files[i]);
        rep.counts.push_back((int)dets.size());
        rep.preTimes.push_back(tm.preprocessMs);
        rep.searchTimes.push_back(tm.searchMs);
        readTimes.push_back(readMs);
        rep.detections.push_back(dets);

        double drawMs = 0;
        if (writeImages && !outDir.empty())
        {
            int64 tVis0 = cv::getTickCount();
            cv::Mat vis = drawResult(img, dets);
            std::string base = files[i];
            size_t sl = base.find_last_of("/\\");
            if (sl != std::string::npos) base = base.substr(sl + 1);
            cv::imwrite(outDir + "/" + base, vis);
            drawMs = tickMs(tVis0);
        }
        drawTimes.push_back(drawMs);

        if (onImage)
        {
            BatchProgress pr;
            pr.file = files[i];
            pr.index = i; pr.total = files.size();
            pr.matches = (int)dets.size();
            pr.preprocessMs = tm.preprocessMs;
            pr.searchMs     = tm.searchMs;
            pr.readMs       = readMs;
            pr.drawMs       = drawMs;
            pr.detections   = &rep.detections.back();
            onImage(pr);
        }
    }

    rep.preprocess = Stats::compute(rep.preTimes);
    rep.search     = Stats::compute(rep.searchTimes);
    std::vector<double> tot;
    tot.reserve(rep.preTimes.size());
    for (size_t i = 0; i < rep.preTimes.size(); i++)
        tot.push_back(rep.preTimes[i] + rep.searchTimes[i]);
    rep.total = Stats::compute(tot);
    rep.read  = Stats::compute(readTimes);
    rep.draw  = Stats::compute(drawTimes);
    return rep;
}

//=======================================================================
//  synthetic angle / scale test
//=======================================================================
GenTestReport Matcher::runGenTest(const std::string& outDir, int n, const Config& cfg)
{
    GenTestReport rep;
    if (!ready() || n <= 0) return rep;

    std::string sdir = outDir + "/_synth";
    cv::utils::fs::createDirectory(sdir);

    const cv::Mat tplG = core_->getModelGray();
    const cv::Point2f tctr = core_->modelCenter();

    struct Case { double aDeg, scale; };
    const std::vector<Case> cases = {
        {   0.0, 1.00 }, {  30.0, 1.00 }, {  45.0, 0.90 },
        {  60.0, 1.00 }, {  90.0, 1.00 }, { 120.0, 1.00 },
        { 150.0, 1.15 }, { 180.0, 1.00 }, { -30.0, 1.00 },
        { -60.0, 0.85 }, { -90.0, 1.00 }, { -135.0, 1.10 }
    };
    const int N = std::min((int)cases.size(), n);
    const int W = 900, H = 900;
    const double cx = W / 2.0, cy = H / 2.0;
    cv::RNG rng(12345);

    //---- generate -------------------------------------------------------
    for (int i = 0; i < N; i++)
    {
        const double a  = cases[i].aDeg * CV_PI / 180.0;
        const double s  = cases[i].scale;
        const double ca = std::cos(a), sa = std::sin(a);
        const double tx = cx - s * (ca * tctr.x + sa * tctr.y);
        const double ty = cy - s * (-sa * tctr.x + ca * tctr.y);
        cv::Mat M = (cv::Mat_<double>(2, 3) <<
            s * ca,  s * sa, tx,
           -s * sa,  s * ca, ty);

        // busy background so the matcher cannot just lock on by chance
        cv::Mat big(H, W, CV_8U, cv::Scalar(40));
        for (int k = 0; k < 8; k++)
        {
            cv::Point p1(rng.uniform(0, W), rng.uniform(0, H));
            cv::Point p2(rng.uniform(0, W), rng.uniform(0, H));
            cv::line(big, p1, p2, cv::Scalar(rng.uniform(50, 200)), 1);
        }
        for (int k = 0; k < 4; k++)
            cv::circle(big, cv::Point(rng.uniform(0, W), rng.uniform(0, H)),
                       rng.uniform(20, 60), cv::Scalar(rng.uniform(60, 200)), 2);

        cv::Mat warped, wmask;
        cv::warpAffine(tplG, warped, M, big.size(),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(40));
        cv::Mat m8u(tplG.size(), CV_8U, cv::Scalar(255));
        cv::warpAffine(m8u, wmask, M, big.size(),
                       cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
        warped.copyTo(big, wmask);

        cv::Mat noise(big.size(), CV_32F);
        cv::randn(noise, 0, 6);
        cv::Mat noise8u;
        noise.convertTo(noise8u, CV_8U);
        big += noise8u;

        char fn[128];
        std::snprintf(fn, sizeof(fn), "a%+05.1f_s%03d.png",
                      cases[i].aDeg, (int)std::round(cases[i].scale * 100));
        cv::imwrite(sdir + "/" + fn, big);
    }

    //---- match and compare ---------------------------------------------
    FindParams fp = cfg.toFindParams();
    fp.maxMatches = 1;
    fp.minScore   = std::min(0.3, fp.minScore);
    fp.useOpenCL  = false;      // run synchronously, avoid the OCL path

    std::vector<std::string> simgs = listImages(sdir, { ".png", ".jpg", ".bmp" });
    std::sort(simgs.begin(), simgs.end());

    std::vector<double> ttimes;
    std::vector<double> errA, errS, errP, scores;

    rep.cases.reserve(simgs.size());
    for (size_t i = 0; i < simgs.size(); i++)
    {
        const std::string& f = simgs[i];
        std::string b = f.substr(f.find_last_of("/\\") + 1);
        int tiA = 0, tiS = 100;
        std::sscanf(b.c_str(), "a%d_s%d.png", &tiA, &tiS);

        cv::Mat img = cv::imread(f, cv::IMREAD_GRAYSCALE);
        if (img.empty()) continue;

        int64 t0 = cv::getTickCount();
        std::vector<MatchResult> r = core_->findShapeModel(img, fp);
        double dt = tickMs(t0);
        ttimes.push_back(dt);

        GenTestReport::Case c;
        c.truthAngleDeg = tiA;
        c.truthScale    = tiS / 100.0;
        c.ms            = dt;

        if (r.empty())
        {
            c.found = false;
            rep.cases.push_back(c);
            continue;
        }
        const MatchResult& m = r[0];
        double ea = m.angle * 180.0 / CV_PI - tiA;
        if (ea >  180) ea -= 360;
        if (ea < -180) ea += 360;

        c.found       = true;
        c.angleDeg    = m.angle * 180.0 / CV_PI;
        c.scale       = m.scale;
        c.score       = m.score;
        c.errAngleDeg = std::fabs(ea);
        c.errScale    = std::fabs(m.scale - c.truthScale);
        c.errPx       = cv::norm(m.pos - cv::Point2f((float)cx, (float)cy));

        errA.push_back(c.errAngleDeg);
        errS.push_back(c.errScale);
        errP.push_back(c.errPx);
        scores.push_back(c.score);
        rep.cases.push_back(c);
    }

    auto mean = [](const std::vector<double>& v)
    {
        if (v.empty()) return 0.0;
        double s = 0; for (double x : v) s += x; return s / v.size();
    };
    auto mx = [](const std::vector<double>& v)
    {
        if (v.empty()) return 0.0;
        return *std::max_element(v.begin(), v.end());
    };

    rep.total          = (int)rep.cases.size();
    rep.found          = (int)scores.size();
    rep.meanErrAngleDeg = mean(errA);  rep.maxErrAngleDeg = mx(errA);
    rep.meanErrScale    = mean(errS);  rep.maxErrScale    = mx(errS);
    rep.meanErrPx       = mean(errP);  rep.maxErrPx       = mx(errP);
    rep.meanScore       = mean(scores);
    rep.minScore        = scores.empty() ? 0.0 : *std::min_element(scores.begin(), scores.end());
    rep.timing          = Stats::compute(ttimes);
    return rep;
}

//=======================================================================
//  self test: cut a patch, model it, find it back after a transform
//=======================================================================
std::vector<Matcher::SelfTestResult> Matcher::runSelfTest(const cv::Mat& gray, const Config& cfg)
{
    std::vector<SelfTestResult> out;
    if (gray.empty()) return out;

    cv::Mat img;
    if (gray.channels() == 3) cv::cvtColor(gray, img, cv::COLOR_BGR2GRAY);
    else gray.convertTo(img, CV_8U);

    const int cw = 154, ch = 148;
    if (img.cols < cw || img.rows < ch) return out;
    const cv::Rect roi((img.cols - cw) / 2, (img.rows - ch) / 2, cw, ch);
    cv::Mat crop = img(roi).clone();

    Config mp = cfg;
    mp.angleStart  = -CV_PI;
    mp.angleExtent = 2 * CV_PI;
    mp.scaleMin    = 0.90;
    mp.scaleMax    = 1.15;
    mp.minContrast = 12;
    mp.maxPoints   = 1200;

    ShapeMatcher m;
    if (!m.createShapeModel(crop, mp.toMatchParams())) return out;

    for (int variant = 0; variant < 3; variant++)
    {
        double trueAngle = 0, trueScale = 1;
        if (variant == 1) trueAngle = 30.0 * CV_PI / 180.0;
        if (variant == 2) { trueAngle = -20.0 * CV_PI / 180.0; trueScale = 1.12; }

        cv::Mat simg = img.clone();
        cv::Point2f imgc(img.cols / 2.f, img.rows / 2.f);
        cv::Mat R = cv::getRotationMatrix2D(imgc, trueAngle * 180 / CV_PI, trueScale);
        if (variant > 0)
            cv::warpAffine(img, simg, R, img.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);

        const cv::Point2f ctr = cv::Point2f((float)roi.x, (float)roi.y) + m.modelCenter();
        cv::Point2f p((float)(R.at<double>(0, 0) * ctr.x + R.at<double>(0, 1) * ctr.y + R.at<double>(0, 2)),
                      (float)(R.at<double>(1, 0) * ctr.x + R.at<double>(1, 1) * ctr.y + R.at<double>(1, 2)));

        FindParams fp;
        fp.minScore = 0.3; fp.maxMatches = 3; fp.minDistance = 40;
        fp.greediness = 0.9; fp.subpixel = 1; fp.useOpenCL = false; fp.debug = 1;
        std::vector<MatchResult> r = m.findShapeModel(simg, fp);

        SelfTestResult res;
        res.variant       = variant;
        res.truthAngleDeg = trueAngle * 180.0 / CV_PI;
        res.truthScale    = trueScale;
        res.truthPos      = p;
        if (!r.empty())
        {
            res.found        = true;
            res.foundPos     = r[0].pos;
            res.foundAngleDeg = r[0].angle * 180.0 / CV_PI;
            res.foundScale   = r[0].scale;
            res.score        = r[0].score;
            res.errPx        = cv::norm(r[0].pos - p);
        }
        res.probeAtTruth = m.probe(simg, p, trueAngle, trueScale);
        out.push_back(res);
    }
    return out;
}

//=======================================================================
//  free helpers
//=======================================================================
std::vector<std::string> listImages(const std::string& dir,
                                    const std::vector<std::string>& exts)
{
    std::vector<std::string> out;
    std::vector<cv::String> files;
    cv::glob(dir, files);
    for (size_t i = 0; i < files.size(); i++)
    {
        std::string f = files[i];
        std::string low = f;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        for (size_t k = 0; k < exts.size(); k++)
            if (low.size() > exts[k].size() &&
                low.compare(low.size() - exts[k].size(), exts[k].size(), exts[k]) == 0)
                out.push_back(f);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool enableOpenCL(bool on)
{
    bool usable = false;
    if (on && cv::ocl::haveOpenCL())
    {
        cv::ocl::Context ctx;
        usable = ctx.ndevices() > 0;
    }
    cv::ocl::setUseOpenCL(usable);
    return usable;
}

//=======================================================================
//  micro benchmark
//=======================================================================
namespace
{
    // sequential read+write of a buffer far bigger than any cache.
    // the scan loop of the matcher is memory bound, so this is the number
    // that decides how fast the search can ever be.
    double benchBandwidth(int threads, size_t bytes)
    {
        const int n = (int)(bytes / sizeof(float));
        cv::Mat a(1, n, CV_32F), b(1, n, CV_32F);
        cv::randu(a, 0.f, 1.f);
        cv::setNumThreads(threads);
        a.copyTo(b);                                   // touch the pages
        cv::setNumThreads(threads);
        double best = 1e30;
        for (int r = 0; r < 3; r++)
        {
            const int64 t0 = cv::getTickCount();
            a.copyTo(b);
            best = std::min(best, tickMs(t0));
        }
        const double moved = 2.0 * bytes;              // read + write
        return moved / (best * 1e-3) / 1e9;            // GB/s
    }

    // two Sobel 3x3 passes over 1280x1024 -> MPix/s of gradient
    double benchSobel(int threads)
    {
        cv::Mat img(1024, 1280, CV_8U);
        cv::randu(img, 0, 256);
        cv::Mat gx, gy;
        cv::setNumThreads(threads);
        cv::Sobel(img, gx, CV_32F, 1, 0, 3);           // warm up
        double best = 1e30;
        for (int r = 0; r < 3; r++)
        {
            const int64 t0 = cv::getTickCount();
            cv::Sobel(img, gx, CV_32F, 1, 0, 3);
            cv::Sobel(img, gy, CV_32F, 0, 1, 3);
            best = std::min(best, tickMs(t0));
        }
        return (1280.0 * 1024.0 * 2.0) / (best * 1e-3) / 1e6;
    }

    // unpacking CV_16F gradients to float, the inner loop of the scan
    double benchHalfCvt(int threads)
    {
        cv::Mat s(1024, 1280, CV_16S);
        cv::randu(s, 0, 1000);
        cv::Mat h, f;
        s.convertTo(h, CV_16F);
        cv::setNumThreads(threads);
        h.convertTo(f, CV_32F);                        // warm up
        double best = 1e30;
        for (int r = 0; r < 3; r++)
        {
            const int64 t0 = cv::getTickCount();
            h.convertTo(f, CV_32F);
            best = std::min(best, tickMs(t0));
        }
        return (1280.0 * 1024.0) / (best * 1e-3) / 1e9;
    }

    std::vector<int> threadCounts(int hw)
    {
        std::vector<int> v;
        v.push_back(1);
        if (hw >= 2) v.push_back(2);
        if (hw >= 4) v.push_back(4);
        if (hw >  4) v.push_back(hw);
        return v;
    }
}

std::string benchmarkReport()
{
    const int hw    = (int)std::thread::hardware_concurrency();
    const int cvCPU = cv::getNumberOfCPUs();
    const int nt0   = cv::getNumThreads();

    std::ostringstream os;
    os << "cores    : hardware_concurrency = " << hw
       << "   cv::getNumberOfCPUs() = " << cvCPU
       << "   cv::getNumThreads() = " << nt0 << "\n";

    const std::vector<int> ts = threadCounts(hw > 0 ? hw : 1);
    const size_t buf = 128u << 20;                     // 128 MB

    os << "\nbandwidth (128 MB copy, read+write) [GB/s]\n";
    for (size_t i = 0; i < ts.size(); i++)
        os << "  " << std::setw(3) << ts[i] << " thread(s): "
           << std::fixed << std::setprecision(2)
           << benchBandwidth(ts[i], buf) << "\n";

    os << "\nSobel 3x3, 1280x1024, 2 passes [MPix/s]\n";
    for (size_t i = 0; i < ts.size(); i++)
        os << "  " << std::setw(3) << ts[i] << " thread(s): "
           << std::fixed << std::setprecision(1)
           << benchSobel(ts[i]) << "\n";

    os << "\nCV_16F -> CV_32F unpack, 1280x1024 [G samples/s]\n";
    for (size_t i = 0; i < ts.size(); i++)
        os << "  " << std::setw(3) << ts[i] << " thread(s): "
           << std::fixed << std::setprecision(3)
           << benchHalfCvt(ts[i]) << "\n";

    cv::setNumThreads(nt0);                            // restore
    return os.str();
}

std::string environmentReport()
{
    std::ostringstream os;
    os << "OpenCV " << CV_VERSION << "\n";
    os << "CPU     : " << cv::getCPUFeaturesLine() << "\n";
#if defined(__AVX2__)
    os << "  -> __AVX2__ defined (256-bit SIMD available)\n";
#endif
#if defined(__F16C__)
    os << "  -> __F16C__ defined (16-bit float conv available)\n";
#endif
#if defined(__SSE4_2__)
    os << "  -> __SSE4_2__ defined (POPCNT available)\n";
#endif
    bool have = cv::ocl::haveOpenCL();
    os << "OpenCL  : haveOpenCL = " << have << "\n";
    if (have)
    {
        cv::ocl::Context ctx;
        os << "           " << ctx.ndevices() << " device(s)\n";
        for (int i = 0; i < ctx.ndevices(); i++)
        {
            cv::ocl::Device d = ctx.device(i);
            os << "             [" << i << "] " << d.name()
               << "  (" << d.version() << ")"
               << "  cu=" << d.maxComputeUnits()
               << "  mem=" << (d.globalMemSize() / (1024 * 1024)) << "MB\n";
        }
    }
    return os.str();
}

std::vector<std::string> excludeFile(const std::vector<std::string>& files,
                                     const std::string& templatePath)
{
    std::string tplName = templatePath;
    size_t ts = tplName.find_last_of("/\\");
    if (ts != std::string::npos) tplName = tplName.substr(ts + 1);

    std::vector<std::string> out;
    out.reserve(files.size());
    for (const std::string& f : files)
    {
        std::string b = f;
        size_t s = b.find_last_of("/\\");
        if (s != std::string::npos) b = b.substr(s + 1);
        if (b != tplName) out.push_back(f);
    }
    return out;
}

} // namespace geomatch
