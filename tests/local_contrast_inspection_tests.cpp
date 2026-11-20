#include "LocalContrastInspection.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace weldscope;

namespace {
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<typename Operation>
void rejects(Operation operation)
{
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid local contrast input accepted");
}
}

int main()
{
    try {
        const cv::Mat uniform(3, 5, CV_8UC3, cv::Scalar::all(150));
        for (const auto mode : {LocalContrastMode::Fast, LocalContrastMode::Precise}) {
            const auto result = inspect_local_contrast(uniform, mode, 12, 0.1);
            check(result.mask.size() == uniform.size() && result.score == 0 && !result.isFail, "Uniform image");
            const auto pixel = inspect_local_contrast(uniform(cv::Rect(0, 0, 1, 1)), mode, 12, 0.1);
            check(pixel.totalPixels == 1 && pixel.candidatePixels == 0, "Single pixel");
        }

        cv::Mat boundary(1, 2, CV_8UC3, cv::Scalar::all(0));
        boundary.col(1).setTo(cv::Scalar::all(27));
        check(inspect_local_contrast(boundary, LocalContrastMode::Fast, 12, 0.1).candidatePixels == 0,
              "Residual equal to the contrast threshold");

        cv::Mat bright(1, 9, CV_8UC3, cv::Scalar::all(0));
        cv::Mat dark(1, 9, CV_8UC3, cv::Scalar::all(90));
        bright.col(4).setTo(cv::Scalar::all(90));
        dark.col(4).setTo(cv::Scalar::all(0));
        const auto before = bright.clone();
        for (const auto& image : {bright, dark}) {
            const auto result = inspect_local_contrast(image, LocalContrastMode::Fast, 12, 1.0 / 9);
            check(result.candidatePixels == 1 && result.mask.at<unsigned char>(0, 4) == 255
                  && result.score == 1.0 / 9 && result.isFail && !result.continuousCandidateRatio,
                  "Bright/dark point and inclusive FAIL threshold");
        }
        const auto point = inspect_local_contrast(bright, LocalContrastMode::Precise, 12, 0.1);
        check(point.candidatePixels == 1 && point.continuousCandidateRatio == 0
              && point.score == 1.0 / 18 && !point.isFail, "Opening removes isolated candidate");
        check(cv::norm(bright, before, cv::NORM_INF) == 0, "Original must remain unchanged");

        cv::Mat plateau(1, 63, CV_8UC3, cv::Scalar::all(0));
        plateau.colRange(24, 39).setTo(cv::Scalar::all(90));
        const auto fast = inspect_local_contrast(plateau, LocalContrastMode::Fast, 12, 0.1);
        const auto precise = inspect_local_contrast(plateau, LocalContrastMode::Precise, 12, 0.1);
        check(fast.mask.at<unsigned char>(0, 31) == 0 && precise.mask.at<unsigned char>(0, 31) == 255
              && precise.continuousCandidateRatio > 0, "Wide change and continuous candidates");

        rejects([] { inspect_local_contrast({}, LocalContrastMode::Fast, 12, 0.1); });
        rejects([&] { inspect_local_contrast(cv::Mat(1, 1, CV_8UC1), LocalContrastMode::Fast, 12, 0.1); });
        rejects([&] { inspect_local_contrast(uniform, LocalContrastMode::Fast, 256, 0.1); });
        rejects([&] {
            inspect_local_contrast(uniform, LocalContrastMode::Fast, 12, std::numeric_limits<double>::quiet_NaN());
        });
        std::cout << "Local contrast checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
