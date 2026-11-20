#pragma once
#include <opencv2/core.hpp>
#include <cstdint>
#include <optional>

namespace weldscope {
enum class LocalContrastMode { Fast, Precise };

struct LocalContrastResult {
    cv::Mat gray;
    cv::Mat mask;
    std::uint64_t candidatePixels;
    std::uint64_t totalPixels;
    double score;
    bool isFail;
    std::optional<double> continuousCandidateRatio;
};

void validate_local_contrast_config(int contrastThreshold, double failRatio);
LocalContrastResult inspect_local_contrast(const cv::Mat& bgr, LocalContrastMode mode,
    int contrastThreshold, double failRatio);
}
