#pragma once

#include <chrono>
#include <functional>
#include <optional>

namespace weldscope {

struct ScoreBand {
    double passMax;
    double failMin;
};

struct AnalysisMeasurement {
    constexpr AnalysisMeasurement(std::optional<double> observedScore, std::chrono::microseconds duration)
        : score(observedScore), elapsed(duration) {}

    std::optional<double> score;
    std::chrono::microseconds elapsed;
};

enum class InspectionOutcome { Pass, Fail, Review, Error };

enum class PolicyReason {
    FastThreshold,
    PreciseThreshold,
    PreciseUncertain,
    BudgetExhausted,
    FastAnalysisError,
    PreciseAnalysisError,
    InvalidFastMeasurement,
    InvalidPreciseMeasurement
};

struct PolicyResult {
    InspectionOutcome outcome;
    PolicyReason reason;
    std::optional<AnalysisMeasurement> precise;
};

class SelectiveInspectionPolicy {
public:
    SelectiveInspectionPolicy(ScoreBand fast, ScoreBand precise, std::chrono::microseconds budget);

    // The callback represents the same input. It returns a missing score for an expected analysis failure.
    PolicyResult evaluate(AnalysisMeasurement fast, const std::function<AnalysisMeasurement()>& analyzePrecise) const;

private:
    ScoreBand fast_;
    ScoreBand precise_;
    std::chrono::microseconds budget_;
};

}
