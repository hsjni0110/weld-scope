#include "BaselineInspection.h"
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <stdexcept>

namespace weldscope {
BaselineConfig::BaselineConfig(int intensityThreshold, double failRatio)
    : intensityThreshold_(intensityThreshold), failRatio_(failRatio)
{
    if (intensityThreshold < 0 || intensityThreshold > 255 || !std::isfinite(failRatio) || failRatio < 0 || failRatio > 1)
        throw std::invalid_argument("Threshold must be an integer in [0,255] and fail_ratio finite in [0,1]");
}

BaselineResult inspect_baseline(const cv::Mat& bgr, const BaselineConfig& config)
{
    if (bgr.empty() || bgr.dims != 2 || bgr.type() != CV_8UC3)
        throw std::invalid_argument("Expected a nonempty 2D 8-bit BGR image");
    BaselineResult result;
    cv::cvtColor(bgr, result.gray, cv::COLOR_BGR2GRAY);
    cv::threshold(result.gray, result.mask, config.intensityThreshold(), 255, cv::THRESH_BINARY_INV);
    result.candidatePixels = static_cast<std::uint64_t>(cv::countNonZero(result.mask));
    result.totalPixels = bgr.total();
    result.score = static_cast<double>(result.candidatePixels) / static_cast<double>(result.totalPixels);
    result.decision = result.score >= config.failRatio() ? BaselineDecision::Fail : BaselineDecision::Pass;
    return result;
}
}
