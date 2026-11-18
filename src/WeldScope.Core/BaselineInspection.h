#pragma once
#include <opencv2/core.hpp>
#include <cstdint>

namespace weldscope {
enum class BaselineDecision { Pass, Fail };

class BaselineConfig {
public:
    BaselineConfig(int intensityThreshold, double failRatio);
    int intensityThreshold() const { return intensityThreshold_; }
    double failRatio() const { return failRatio_; }
private:
    int intensityThreshold_;
    double failRatio_;
};

struct BaselineResult {
    cv::Mat gray;
    cv::Mat mask;
    std::uint64_t candidatePixels;
    std::uint64_t totalPixels;
    double score;
    BaselineDecision decision;
};

BaselineResult inspect_baseline(const cv::Mat& bgr, const BaselineConfig& config);
}
