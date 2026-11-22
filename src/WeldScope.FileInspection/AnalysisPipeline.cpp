#include "AnalysisPipeline.h"
#include "PooledSmoothModel.h"

#include <cmath>
#include <stdexcept>

namespace weldscope {

InspectionOutcome classify_score(double score, ScoreBand band)
{
    if (!std::isfinite(score) || score < 0 || score > 1) return InspectionOutcome::Error;
    if (score <= band.passMax) return InspectionOutcome::Pass;
    if (score >= band.failMin) return InspectionOutcome::Fail;
    return InspectionOutcome::Review;
}

const char* outcome_name(InspectionOutcome outcome)
{
    switch (outcome) {
    case InspectionOutcome::Pass: return "PASS";
    case InspectionOutcome::Fail: return "FAIL";
    case InspectionOutcome::Review: return "REVIEW";
    case InspectionOutcome::Error: return "ERROR";
    }
    throw std::logic_error("Unknown inspection outcome");
}

AnalysisOutput analyze_image(const cv::Mat& bgr, const PooledSmoothModel& model)
{
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::optional<double> score;
    try {
        score = model.score(bgr);
    } catch (const cv::Exception&) {
    } catch (const std::invalid_argument&) {}
    const auto outcome = score ? classify_score(*score, model.band(bgr.cols)) : InspectionOutcome::Error;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
    if (outcome == InspectionOutcome::Error) return {std::nullopt, outcome, "analysis", elapsed};
    return {score, outcome, outcome == InspectionOutcome::Review ? "score_between_limits" : "score_threshold", elapsed};
}

}
