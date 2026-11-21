#pragma once

#include <opencv2/core.hpp>
#include <array>
#include <filesystem>
#include <vector>

namespace pattern_study {
inline constexpr std::array<int, 3> lengths{32, 96, 192};
using Features = std::array<double, 192>;
struct Model {
    std::vector<double> mean, scale, weights;
    double intercept = 0;
    double score(const Features& values) const;
};
Features extract(const cv::Mat& image, int length);
std::array<Model, 3> load(const cv::FileStorage& file);
void train(const std::vector<Features>& rows, const std::vector<int>& labels,
           const std::filesystem::path& destination, bool checkConvergence);
}
