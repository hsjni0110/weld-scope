#include "LocalContrastInspection.h"
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <stdexcept>

namespace weldscope {
namespace {
cv::Mat candidate_mask(const cv::Mat& gray, int windowWidth, int contrastThreshold)
{
    cv::Mat mask;
    cv::blur(gray, mask, cv::Size(windowWidth, 1), cv::Point(-1, -1), cv::BORDER_REPLICATE);
    cv::absdiff(gray, mask, mask);
    cv::threshold(mask, mask, contrastThreshold, 255, cv::THRESH_BINARY);
    return mask;
}
}

void validate_local_contrast_config(int contrastThreshold, double failRatio)
{
    if (contrastThreshold < 0 || contrastThreshold > 255 || !std::isfinite(failRatio) || failRatio < 0 || failRatio > 1)
        throw std::invalid_argument("contrast_threshold must be an integer in [0,255] and fail_ratio finite in [0,1]");
}

LocalContrastResult inspect_local_contrast(const cv::Mat& bgr, LocalContrastMode mode,
    int contrastThreshold, double failRatio)
{
    if (bgr.empty() || bgr.dims != 2 || bgr.type() != CV_8UC3)
        throw std::invalid_argument("Expected a nonempty 2D 8-bit BGR image");
    validate_local_contrast_config(contrastThreshold, failRatio);

    LocalContrastResult result{};
    cv::cvtColor(bgr, result.gray, cv::COLOR_BGR2GRAY);
    result.mask = candidate_mask(result.gray, 9, contrastThreshold);
    result.totalPixels = bgr.total();
    const double pixelCount = static_cast<double>(result.totalPixels);
    if (mode == LocalContrastMode::Precise) {
        const auto wideMask = candidate_mask(result.gray, 31, contrastThreshold);
        cv::bitwise_or(result.mask, wideMask, result.mask);
        cv::Mat continuousMask;
        const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 1));
        cv::morphologyEx(result.mask, continuousMask, cv::MORPH_OPEN, kernel, cv::Point(-1, -1),
            1, cv::BORDER_CONSTANT, cv::Scalar(0));
        result.continuousCandidateRatio = cv::countNonZero(continuousMask) / pixelCount;
    }
    result.candidatePixels = static_cast<std::uint64_t>(cv::countNonZero(result.mask));
    result.score = static_cast<double>(result.candidatePixels) / pixelCount;
    if (result.continuousCandidateRatio)
        result.score = (result.score + *result.continuousCandidateRatio) / 2;
    result.isFail = result.score >= failRatio;
    return result;
}
}
