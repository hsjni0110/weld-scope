#include "SelectiveInspectionPolicy.h"

#include <stdexcept>

namespace weldscope {
namespace {

using Outcome = InspectionOutcome;
using Reason = PolicyReason;
using namespace std::chrono_literals;

bool valid_band(ScoreBand band)
{
    return 0 <= band.passMax && band.passMax < band.failMin && band.failMin <= 1;
}

bool valid_measurement(const AnalysisMeasurement& measurement)
{
    return measurement.elapsed >= 0us && measurement.fraction() >= 0ns
        && (!measurement.score || (0 <= *measurement.score && *measurement.score <= 1));
}

}

InspectionOutcome SelectiveInspectionPolicy::classify_score(double score, ScoreBand band)
{
    if (score <= band.passMax) return Outcome::Pass;
    if (score >= band.failMin) return Outcome::Fail;
    return Outcome::Review;
}

SelectiveInspectionPolicy::SelectiveInspectionPolicy(ScoreBand fast, ScoreBand precise,
    std::chrono::microseconds budget)
    : fast_(fast), precise_(precise), budget_(budget)
{
    if (!valid_band(fast_) || !valid_band(precise_) || budget_.count() <= 0)
        throw std::invalid_argument("Expected ordered score bands in [0,1] and a positive analysis budget");
}

PolicyResult SelectiveInspectionPolicy::evaluate(AnalysisMeasurement fast,
    const std::function<AnalysisMeasurement()>& analyzePrecise) const
{
    if (!valid_measurement(fast)) return {Outcome::Error, Reason::InvalidFastMeasurement, std::nullopt};
    if (!fast.score) return {Outcome::Error, Reason::FastAnalysisError, std::nullopt};
    if (fast.elapsed > budget_ || (fast.elapsed == budget_ && fast.fraction() > 0ns))
        return {Outcome::Review, Reason::BudgetExhausted, std::nullopt};

    const auto fastOutcome = classify_score(*fast.score, fast_);
    if (fastOutcome != Outcome::Review) return {fastOutcome, Reason::FastThreshold, std::nullopt};

    const auto remaining = budget_ - fast.elapsed;
    if (remaining == 0us) return {Outcome::Review, Reason::BudgetExhausted, std::nullopt};
    if (!analyzePrecise) return {Outcome::Error, Reason::PreciseAnalysisError, std::nullopt};

    const auto precise = analyzePrecise();
    if (!valid_measurement(precise)) return {Outcome::Error, Reason::InvalidPreciseMeasurement, precise};
    if (!precise.score) return {Outcome::Error, Reason::PreciseAnalysisError, precise};
    const auto fractionalTime = std::chrono::ceil<std::chrono::microseconds>(fast.fraction() + precise.fraction());
    if (precise.elapsed > remaining || fractionalTime > remaining - precise.elapsed)
        return {Outcome::Review, Reason::BudgetExhausted, precise};

    const auto preciseOutcome = classify_score(*precise.score, precise_);
    const auto reason = preciseOutcome == Outcome::Review ? Reason::PreciseUncertain : Reason::PreciseThreshold;
    return {preciseOutcome, reason, precise};
}

}
