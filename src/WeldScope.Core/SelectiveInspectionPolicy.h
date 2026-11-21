#pragma once

#include <chrono>
#include <functional>
#include <optional>

namespace weldscope {

// passMax 이하는 통과, failMin 이상은 불량, 그 사이는 검토 대상으로 삼는다.
struct ScoreBand {
    double passMax;
    double failMin;
};

// 한 분석의 점수와 소요 시간. 점수가 없으면 분석 중 예상된 실패를 뜻한다.
struct AnalysisMeasurement {
    // 마이크로초로 측정된 결과를 기록한다.
    constexpr AnalysisMeasurement(std::optional<double> observedScore, std::chrono::microseconds duration)
        : score(observedScore), elapsed(duration) {}

    // 나노초로 잰 시간을 받아 마이크로초 미만의 나머지까지 보관한다.
    static constexpr AnalysisMeasurement from_nanoseconds(std::optional<double> score,
        std::chrono::nanoseconds duration)
    {
        const auto whole = std::chrono::duration_cast<std::chrono::microseconds>(duration);
        auto measurement = AnalysisMeasurement(score, whole);
        measurement.fraction_ = duration - whole;
        return measurement;
    }

    // elapsed에 포함되지 않은 마이크로초 미만의 시간이다.
    constexpr std::chrono::nanoseconds fraction() const { return fraction_; }

    std::optional<double> score;
    std::chrono::microseconds elapsed;

private:
    // 큰 시간을 나노초로 변환하지 않고 마이크로초가 표현할 수 있는 범위를 유지한다.
    std::chrono::nanoseconds fraction_{};
};

// 최종 판정에서 사람의 검토가 필요한 경우와 분석이 실패한 경우를 구분한다.
enum class InspectionOutcome { Pass, Fail, Review, Error };

// 판정을 내린 단계나 판정을 보류한 원인을 기록한다.
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

// 호출자가 판정과 그 이유를 함께 표시하거나 저장할 수 있는 값. 정밀 분석을 실행한 경우만 precise가 채워진다.
struct PolicyResult {
    InspectionOutcome outcome;
    PolicyReason reason;
    std::optional<AnalysisMeasurement> precise;
};

// 점수와 측정 시간을 바탕으로 추가 분석 여부와 최종 판정을 결정한다.
class SelectiveInspectionPolicy {
public:
    // 0 <= passMax < failMin <= 1인 두 구간과 양수인 시간 한도를 받는다. 잘못된 값은 예외로 알린다.
    SelectiveInspectionPolicy(ScoreBand fast, ScoreBand precise, std::chrono::microseconds budget);

    // 빠른 점수가 검토 구간에 있고 시간이 남은 경우에만 같은 입력의 정밀 분석을 한 번 호출한다.
    // 콜백은 예상된 실패를 점수 없음으로 반환한다. 시간 한도는 분석이 끝난 뒤 확인한다.
    PolicyResult evaluate(AnalysisMeasurement fast, const std::function<AnalysisMeasurement()>& analyzePrecise) const;

    // 점수와 구간이 이미 검증된 경우에만 호출한다. 시간 한도는 여기서 평가하지 않는다.
    static InspectionOutcome classify_score(double score, ScoreBand band);

private:
    ScoreBand fast_;
    ScoreBand precise_;
    std::chrono::microseconds budget_;
};

}
