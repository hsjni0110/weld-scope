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

    static constexpr AnalysisMeasurement from_nanoseconds(std::optional<double> score,
        std::chrono::nanoseconds duration)
    {
        const auto whole = std::chrono::duration_cast<std::chrono::microseconds>(duration);
        auto measurement = AnalysisMeasurement(score, whole);
        measurement.fraction_ = duration - whole;
        return measurement;
    }

    constexpr std::chrono::nanoseconds fraction() const { return fraction_; }

    std::optional<double> score;
    std::chrono::microseconds elapsed;

private:
    // Keep the full microsecond range without converting large durations to nanoseconds.
    std::chrono::nanoseconds fraction_{};
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

    // The observation and band have already passed policy validation.
    static InspectionOutcome classify_score(double score, ScoreBand band);

private:
    ScoreBand fast_;
    ScoreBand precise_;
    std::chrono::microseconds budget_;
};

}
