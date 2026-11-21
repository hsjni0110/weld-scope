#include "AnalysisPipeline.h"
#include "LocalContrastInspection.h"
#include "PooledSmoothModel.h"

#include <utility>

namespace weldscope {
namespace {
using Clock = std::chrono::steady_clock;
using Outcome = InspectionOutcome;

double read_number(const cv::FileStorage& json, const char* field)
{
    const auto value = json[field];
    if (!value.isInt() && !value.isReal())
        throw std::invalid_argument(std::string("Missing or nonnumeric field: ") + field);
    return static_cast<double>(value);
}

const char* policy_reason(PolicyReason reason)
{
    switch (reason) {
    case PolicyReason::FastThreshold: return "fast_threshold";
    case PolicyReason::PreciseThreshold: return "precise_threshold";
    case PolicyReason::PreciseUncertain: return "precise_uncertain";
    case PolicyReason::BudgetExhausted: return "budget_exhausted";
    case PolicyReason::FastAnalysisError: return "fast_analysis_error";
    case PolicyReason::PreciseAnalysisError: return "precise_analysis_error";
    case PolicyReason::InvalidFastMeasurement: return "invalid_fast_measurement";
    case PolicyReason::InvalidPreciseMeasurement: return "invalid_precise_measurement";
    }
    throw std::logic_error("Unknown inspection policy reason");
}

void put_candidate(AnalysisOutput& output, LocalContrastResult result)
{
    output.gray = std::move(result.gray);
    output.mask = std::move(result.mask);
    output.candidatePixels = result.candidatePixels;
    output.totalPixels = result.totalPixels;
    output.score = result.score;
    output.continuousCandidateRatio = result.continuousCandidateRatio;
}

AnalysisMeasurement observe_local(AnalysisOutput& output, const cv::Mat& bgr, int threshold,
    LocalContrastMode mode, ScoreBand band)
{
    const bool precise = mode == LocalContrastMode::Precise;
    const char* stage = precise ? "PRECISE" : "FAST";
    const auto start = Clock::now();
    std::optional<LocalContrastResult> candidate;
    try {
        candidate = inspect_local_contrast(bgr, mode, threshold, band.failMin);
    } catch (const cv::Exception&) {
    } catch (const std::invalid_argument&) {}
    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
    output.analysisMs += std::chrono::duration<double, std::milli>(duration).count();
    if (!candidate) {
        output.attempts.push_back({stage, std::nullopt, Outcome::Error,
            precise ? "precise_analysis_error" : "fast_analysis_error", duration});
        return AnalysisMeasurement::from_nanoseconds(std::nullopt, duration);
    }
    const double score = candidate->score;
    const auto outcome = SelectiveInspectionPolicy::classify_score(score, band);
    const char* reason = outcome == Outcome::Review
        ? (precise ? "precise_uncertain" : "fast_uncertain")
        : (precise ? "precise_threshold" : "fast_threshold");
    put_candidate(output, std::move(*candidate));
    output.attempts.push_back({stage, score, outcome, reason, duration});
    return AnalysisMeasurement::from_nanoseconds(score, duration);
}
}

InspectionParameters parse_inspection_parameters(const cv::FileStorage& json, InspectionMode mode)
{
    InspectionParameters parameters{mode, 0, 0, {}, {}, std::chrono::microseconds(0)};
    if (mode == InspectionMode::Baseline) {
        const auto baseline = parse_baseline_config(json);
        parameters.threshold = baseline.intensityThreshold();
        parameters.failRatio = baseline.failRatio();
    } else if (mode == InspectionMode::Selective || mode == InspectionMode::PooledSmooth) {
        if (read_integer(json["schema_version"], "schema_version") != 1)
            throw std::invalid_argument("Unsupported config schema");
        parameters.threshold = read_integer(json["contrast_threshold"], "contrast_threshold");
        parameters.fastBand = {read_number(json, "fast_pass_max"), read_number(json, "fast_fail_min")};
        parameters.preciseBand = {read_number(json, "precise_pass_max"), read_number(json, "precise_fail_min")};
        parameters.budget = std::chrono::microseconds(read_integer(json["budget_us"], "budget_us"));
        validate_local_contrast_config(parameters.threshold, 0);
        (void)SelectiveInspectionPolicy(parameters.fastBand, parameters.preciseBand, parameters.budget);
    } else {
        const auto settings = read_inspection_settings(json, "contrast_threshold");
        validate_local_contrast_config(settings.threshold, settings.failRatio);
        parameters.threshold = settings.threshold;
        parameters.failRatio = settings.failRatio;
    }
    return parameters;
}

const char* inspection_version(InspectionMode mode)
{
    switch (mode) {
    case InspectionMode::Baseline: return "global-dark-ratio-v1";
    case InspectionMode::Fast: return "local-contrast-fast-v1";
    case InspectionMode::Precise: return "local-contrast-precise-v1";
    case InspectionMode::Selective: return "local-contrast-selective-v1";
    case InspectionMode::PooledSmooth: return "openzdm-pooled-smooth-220-v1";
    }
    throw std::logic_error("Unknown inspection mode");
}

const char* outcome_name(InspectionOutcome outcome)
{
    switch (outcome) {
    case Outcome::Pass: return "PASS";
    case Outcome::Fail: return "FAIL";
    case Outcome::Review: return "REVIEW";
    case Outcome::Error: return "ERROR";
    }
    throw std::logic_error("Unknown inspection outcome");
}

AnalysisOutput analyze_image(const cv::Mat& bgr, const InspectionParameters& parameters,
    const std::function<void()>& beforePrecise, const PooledSmoothModel* model)
{
    AnalysisOutput output;
    if ((parameters.mode == InspectionMode::PooledSmooth) != (model != nullptr))
        throw std::invalid_argument("Frozen model registration mismatch");
    if (parameters.mode == InspectionMode::Selective || model) {
        // The frozen first-stage budget includes width calibration.
        const auto start = model ? Clock::now() : Clock::time_point{};
        const auto band = model ? model->band(bgr.cols) : parameters.fastBand;
        const SelectiveInspectionPolicy policy(band, parameters.preciseBand, parameters.budget);
        const auto fast = [&] {
            if (!model) return observe_local(output, bgr, parameters.threshold, LocalContrastMode::Fast, band);
            std::optional<double> score;
            try { score = model->score(bgr); }
            catch (const std::invalid_argument&) {}
            catch (const cv::Exception&) {}
            const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start);
            output.analysisMs = std::chrono::duration<double, std::milli>(duration).count();
            output.score = score;
            const auto outcome = score ? SelectiveInspectionPolicy::classify_score(*score, band) : Outcome::Error;
            output.attempts.push_back({"POOLED_WIDTH", score, outcome, !score ? "fast_analysis_error" :
                outcome == Outcome::Review ? "fast_uncertain" : "fast_threshold", duration});
            return AnalysisMeasurement::from_nanoseconds(score, duration);
        }();
        const auto result = policy.evaluate(fast, [&] {
            if (beforePrecise) beforePrecise();
            return observe_local(output, bgr, parameters.threshold, LocalContrastMode::Precise, parameters.preciseBand);
        });
        output.outcome = result.outcome;
        output.reason = policy_reason(result.reason);
        return output;
    }

    const char* stage = "BASELINE";
    Clock::duration duration;
    if (parameters.mode == InspectionMode::Baseline) {
        const BaselineConfig config(parameters.threshold, parameters.failRatio);
        const auto start = Clock::now();
        auto baseline = inspect_baseline(bgr, config);
        duration = Clock::now() - start;
        output.gray = std::move(baseline.gray);
        output.mask = std::move(baseline.mask);
        output.candidatePixels = baseline.candidatePixels;
        output.totalPixels = baseline.totalPixels;
        output.score = baseline.score;
        output.outcome = baseline.decision == BaselineDecision::Fail ? Outcome::Fail : Outcome::Pass;
        output.reason = "dark_pixel_ratio";
    } else {
        const bool precise = parameters.mode == InspectionMode::Precise;
        const auto start = Clock::now();
        auto local = inspect_local_contrast(bgr, precise ? LocalContrastMode::Precise : LocalContrastMode::Fast,
            parameters.threshold, parameters.failRatio);
        duration = Clock::now() - start;
        output.outcome = local.isFail ? Outcome::Fail : Outcome::Pass;
        output.reason = precise ? "precise_threshold" : "fast_threshold";
        stage = precise ? "PRECISE" : "FAST";
        put_candidate(output, std::move(local));
    }
    output.analysisMs = std::chrono::duration<double, std::milli>(duration).count();
    output.attempts.push_back({stage, output.score, output.outcome, output.reason,
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration)});
    return output;
}

}
