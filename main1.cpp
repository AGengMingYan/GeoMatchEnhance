#include <iostream>
#include <opencv2/core/cvdef.h>
#include <opencv2/highgui.hpp>
#include "GeoMatch/GeoMatch.h"

using namespace geomatch;

void t1() {
    // 创建模板
    Config cfg;
    // cfg.minContrast = 100;
    cfg.maxPoints = 1200;
    cv::Mat tpl = cv::imread("case1/temp.jpg", cv::IMREAD_GRAYSCALE);
    cfg.angleStart = -5 * CV_PI / 360.0;
    cfg.angleExtent = 5 * CV_PI / 180.0;
    cfg.scaleMin = 1;
    cfg.scaleMax = 1;
    cfg.numLevels = 3;
    Matcher m;
    if (!m.create(tpl, cfg))
    {
        std::cerr << "[t1] failed to create the shape model\n";
        return ;
    }
    cv::Mat info = m.drawModelInfo("");
    m.save("t1.shm");
    cv::imshow("", info);
    cv::waitKey(0);
}
void t2() {
    Matcher m;
    if (!m.load("t1.shm"))
    {
        std::cerr << "[t2] cannot load model: " << "t1.shm" << "\n";
        return ;
    }
    cv::Mat img = cv::imread("case1/tmp_grab_1762994464.jpg", cv::IMREAD_GRAYSCALE);
    cv::Mat drawImg = img.clone();
    Config cfg = m.config();
    cfg.minScore = 0.7;
    cfg.maxMatches = 1;
    cfg.useOpenCL = false;
    m.setConfig(cfg);
    Timing tm;
    std::vector<Detection> dets;
    for (int i = 0; i < 1; i++)
    {
        int64 t0 = cv::getTickCount();
        dets = m.detect(img, &tm);
        const double wall = (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();
        std::cout << "run#" << i
                  << "  wall=" << std::fixed << std::setprecision(2) << wall
                  << "  pre=" << tm.preprocessMs
                  << "  search=" << tm.searchMs
                  << "  rec=" << tm.recognitionMs()
                  << "  GAP=" << wall - tm.recognitionMs() << "\n";
    }
    for (size_t k = 0; k < dets.size(); k++)
    {
        const Detection& d = dets[k];
        std::cout << "        #" << k
                    << "  score=" << std::setprecision(3) << d.score
                    << "  pos=(" << (int)d.pos.x << "," << (int)d.pos.y << ")"
                    << "  angle=" << std::setprecision(1) << (d.angle * 180.0 / CV_PI)
                    << "deg  scale=" << std::setprecision(3) << d.scale << "\n";
    }
    cv::Mat vis = m.drawResult(img, dets);
    imshow("", vis);

    auto pos = m.getDrawResult(dets);
    for(cv::Point2d p : pos) {
        cv::circle(drawImg, p, 1,
            cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    }
    cv::imshow("points", drawImg);

    cv::waitKey(0);

}

int main() {
    t2();
    return 0;
}
