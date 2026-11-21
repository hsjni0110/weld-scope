#pragma once

#include "InspectionInput.h"
#include "SelectiveInspectionPolicy.h"

#include <cstdint>

namespace weldscope {

enum class InspectionMode { Baseline, Fast, Precise, Selective, PooledSmooth };
inline constexpr auto pooledSmoothConfigFile = L"pooled-smooth-220.json";
class PooledSmoothModel;

struct InspectionParameters {
    InspectionMode mode;
    int threshold;
    double failRatio;
    ScoreBand fastBand;
    ScoreBand preciseBand;
    std::chrono::microseconds budget;
};

struct InspectionAttempt {
    std::string stage;
    std::optional<double> score;
    InspectionOutcome outcome;
    std::string reason;
    std::chrono::nanoseconds elapsed;
};

struct AnalysisOutput {
    cv::Mat gray, mask;
    std::uint64_t candidatePixels = 0, totalPixels = 0;
    std::optional<double> score, continuousCandidateRatio;
    InspectionOutcome outcome = InspectionOutcome::Error;
    std::string reason;
    double analysisMs = 0;
    std::vector<InspectionAttempt> attempts;
};

InspectionParameters parse_inspection_parameters(const cv::FileStorage& json, InspectionMode mode);
const char* inspection_version(InspectionMode mode);
const char* outcome_name(InspectionOutcome outcome);
AnalysisOutput analyze_image(const cv::Mat& bgr, const InspectionParameters& parameters,
    const std::function<void()>& beforePrecise = {}, const PooledSmoothModel* model = nullptr);

}
