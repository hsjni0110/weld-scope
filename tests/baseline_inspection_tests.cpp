#include "BaselineInspection.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(reason);
}
template<class Action> void rejects(Action action, const char* reason)
{
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(reason);
}
int main()
{
    using namespace weldscope;
    try {
        cv::Mat input(2, 2, CV_8UC3);
        const unsigned char values[] = {0, 128, 129, 255};
        for (int i = 0; i < 4; ++i) input.at<cv::Vec3b>(i / 2, i % 2) = cv::Vec3b::all(values[i]);
        const auto original = input.clone();
        auto result = inspect_baseline(input, BaselineConfig(128, 0.5));
        require(result.candidatePixels == 2 && result.totalPixels == 4, "threshold includes equality, excludes next value");
        require(result.score == 0.5 && result.decision == BaselineDecision::Fail, "ratio equality fails");
        require(result.mask.at<unsigned char>(0, 1) == 255 && result.mask.at<unsigned char>(1, 0) == 0, "mask marks actual threshold pixels");
        require(result.gray.size() == input.size() && result.mask.size() == input.size(), "native dimensions preserved");
        require(cv::norm(input, original) == 0, "input remains unchanged");
        require(inspect_baseline(input, BaselineConfig(128, 0.5001)).decision == BaselineDecision::Pass, "ratio above score passes");
        for (const auto& invalid : {cv::Mat(), cv::Mat(1, 1, CV_16UC3), cv::Mat(1, 1, CV_8UC1), cv::Mat(1, 1, CV_8UC4)})
            rejects([&] { inspect_baseline(invalid, BaselineConfig(128, 0.5)); }, "unsupported input rejected");
        for (int threshold : {-1, 256}) rejects([&] { BaselineConfig config(threshold, 0.5); }, "pixel threshold validated");
        for (double ratio : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
            rejects([&] { BaselineConfig config(128, ratio); }, "finite ratio range validated");
        require(inspect_baseline(input, BaselineConfig(0, 0)).decision == BaselineDecision::Fail, "valid zero ratio");
        require(inspect_baseline(input, BaselineConfig(255, 1)).score == 1, "upper boundaries accepted");
        std::cout << "PASS: candidate pixels, threshold and ratio boundaries, dimensions, ownership, invalid input/config\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
