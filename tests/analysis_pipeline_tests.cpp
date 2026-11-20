#include "AnalysisPipeline.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace weldscope;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_shared_analysis() {
    const cv::Mat image(4, 20, CV_8UC3, cv::Scalar(220, 220, 220));
    InspectionParameters parameters{InspectionMode::Baseline, 128, 0.5, {}, {}, 0us};
    const auto baseline = analyze_image(image, parameters);
    require(baseline.outcome == InspectionOutcome::Pass && baseline.score == 0.0, "baseline verdict and score");
    require(baseline.attempts.size() == 1 && baseline.attempts.front().stage == "BASELINE", "baseline attempt");

    parameters = {InspectionMode::Selective, 12, 0.10, {0.1, 0.9}, {0.1, 0.9}, 1s};
    int callbacks = 0;
    const auto fast = analyze_image(image, parameters, [&] { ++callbacks; });
    require(fast.outcome == InspectionOutcome::Pass && callbacks == 0 &&
        fast.attempts.size() == 1 && fast.attempts.front().stage == "FAST", "confident fast analysis");

    cv::Mat stripes(4, 20, CV_8UC3, cv::Scalar(0, 0, 0));
    stripes.col(10).setTo(cv::Scalar(255, 255, 255));
    auto probe = analyze_image(stripes, {InspectionMode::Fast, 12, 0.1, {}, {}, 0us});
    require(probe.score && *probe.score > 0 && *probe.score < 1, "test image must have intermediate fast score");
    parameters.fastBand = {0.0, 1.0};
    const auto precise = analyze_image(stripes, parameters, [&] { ++callbacks; });
    require(callbacks == 1 && precise.attempts.size() == 2 && precise.attempts.back().stage == "PRECISE",
        "one precise attempt on the same input");
    require(precise.attempts.front().outcome == InspectionOutcome::Review, "uncertain fast attempt");
    const auto unsupported = analyze_image(cv::Mat{}, parameters, [&] { ++callbacks; });
    require(unsupported.outcome == InspectionOutcome::Error && unsupported.reason == "fast_analysis_error" &&
        !unsupported.score && unsupported.attempts.size() == 1 && callbacks == 1, "fast failure skips precise work");
    try {
        analyze_image(stripes, parameters, [] { throw std::runtime_error("cancelled at callback"); });
        require(false, "callback cancellation propagated");
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "cancelled at callback", "callback exception identity");
    }
}

void test_selective_config() {
    const std::string json = R"({"schema_version":1,"contrast_threshold":12,"fast_pass_max":0.1,"fast_fail_min":0.9,
        "precise_pass_max":0.2,"precise_fail_min":0.8,"budget_us":150})";
    const auto parsed = parse_inspection_parameters(read_json({json.begin(), json.end()}), InspectionMode::Selective);
    require(parsed.budget == 150us && parsed.threshold == 12 &&
        parsed.fastBand.passMax == 0.1 && parsed.preciseBand.failMin == 0.8, "selective settings");
    try {
        parse_inspection_parameters(read_json({json.begin(), json.end()}), InspectionMode::Baseline);
        require(false, "selective settings rejected in baseline mode");
    } catch (const std::invalid_argument&) {}
}
}

int main() {
    try {
        test_shared_analysis();
        test_selective_config();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
