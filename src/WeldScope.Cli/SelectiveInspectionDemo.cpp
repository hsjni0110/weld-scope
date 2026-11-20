#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "SelectiveInspectionPolicy.h"

#include <iostream>

namespace {
using namespace weldscope;
using namespace std::chrono_literals;
using Outcome = InspectionOutcome;
using Reason = PolicyReason;

struct Sample {
    const char* name;
    AnalysisMeasurement fast;
    AnalysisMeasurement precise;
    Outcome expectedOutcome;
    Reason expectedReason;
    int expectedCalls;
};

const char* label(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Pass: return "PASS";
    case Outcome::Fail: return "FAIL";
    case Outcome::Review: return "REVIEW";
    case Outcome::Error: return "ERROR";
    }
    return "UNKNOWN";
}

const char* label(Reason reason)
{
    switch (reason) {
    case Reason::FastThreshold: return "FAST_THRESHOLD";
    case Reason::PreciseThreshold: return "PRECISE_THRESHOLD";
    case Reason::PreciseUncertain: return "PRECISE_UNCERTAIN";
    case Reason::BudgetExhausted: return "BUDGET_EXHAUSTED";
    case Reason::FastAnalysisError: return "FAST_ANALYSIS_ERROR";
    case Reason::PreciseAnalysisError: return "PRECISE_ANALYSIS_ERROR";
    case Reason::InvalidFastMeasurement: return "INVALID_FAST_MEASUREMENT";
    case Reason::InvalidPreciseMeasurement: return "INVALID_PRECISE_MEASUREMENT";
    }
    return "UNKNOWN";
}

void print_measurement(const char* stage, const AnalysisMeasurement& measurement)
{
    std::cout << ' ' << stage << "_score=";
    if (measurement.score) std::cout << *measurement.score;
    else std::cout << '-';
    std::cout << ' ' << stage << "_us=" << measurement.elapsed.count();
}
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    constexpr ScoreBand fastBand{0.25, 0.75};
    constexpr ScoreBand preciseBand{0.125, 0.875};
    constexpr auto budget = 10000us;
    const SelectiveInspectionPolicy policy(fastBand, preciseBand, budget);
    const AnalysisMeasurement uncertainFast{0.5, 2000us};
    constexpr AnalysisMeasurement unusedPrecise{std::nullopt, 0us};
    const Sample samples[]{
        {"빠른 정상 경계", {0.25, 2000us}, unusedPrecise, Outcome::Pass, Reason::FastThreshold, 0},
        {"빠른 불량 경계", {0.75, 2000us}, unusedPrecise, Outcome::Fail, Reason::FastThreshold, 0},
        {"정밀 정상 경계 및 예산 도달", uncertainFast, {0.125, 8000us}, Outcome::Pass, Reason::PreciseThreshold, 1},
        {"정밀 불량 경계", uncertainFast, {0.875, 3000us}, Outcome::Fail, Reason::PreciseThreshold, 1},
        {"정밀 점수 불확실", uncertainFast, {0.5, 3000us}, Outcome::Review, Reason::PreciseUncertain, 1},
        {"정밀 분석 시작 전 예산 소진", {0.5, budget}, unusedPrecise, Outcome::Review, Reason::BudgetExhausted, 0},
        {"정밀 분석 1µs 초과", uncertainFast, {0.875, 8001us}, Outcome::Review, Reason::BudgetExhausted, 1},
        {"정밀 분석 실패", uncertainFast, {std::nullopt, 3000us}, Outcome::Error, Reason::PreciseAnalysisError, 1},
    };
    std::cout << "[샘플] 선택적 정밀 분석\n"
              << "fast_pass_max=" << fastBand.passMax << " fast_fail_min=" << fastBand.failMin
              << " precise_pass_max=" << preciseBand.passMax << " precise_fail_min=" << preciseBand.failMin
              << " budget_us=" << budget.count() << '\n';

    for (const auto& sample : samples) {
        std::cout << '[' << sample.name << ']';
        print_measurement("fast", sample.fast);
        std::cout << '\n';
        int calls = 0;
        const auto result = policy.evaluate(sample.fast, [&] {
            ++calls;
            std::cout << "  state=REANALYZE selection=FAST_UNCERTAIN\n" << std::flush;
            return sample.precise;
        });
        std::cout << "  precise_calls=" << calls;
        if (result.precise) print_measurement("precise", *result.precise);
        std::cout << " outcome=" << label(result.outcome) << " reason=" << label(result.reason) << '\n';
        if (calls != sample.expectedCalls || result.outcome != sample.expectedOutcome
            || result.reason != sample.expectedReason || result.precise.has_value() != (calls == 1)) {
            std::cerr << "샘플 검증 실패\n";
            return 1;
        }
    }
}
