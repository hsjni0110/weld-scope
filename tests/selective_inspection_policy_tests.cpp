#include "SelectiveInspectionPolicy.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using namespace weldscope;
using namespace std::chrono_literals;
using Outcome = InspectionOutcome;
using Reason = PolicyReason;

template<typename T, typename = void>
struct AcceptsScoreWithoutTime : std::false_type {};

template<typename T>
struct AcceptsScoreWithoutTime<T, std::void_t<decltype(T{0.5})>> : std::true_type {};

static_assert(!std::is_default_constructible_v<AnalysisMeasurement>, "An observation requires elapsed time");
static_assert(!AcceptsScoreWithoutTime<AnalysisMeasurement>::value, "A score alone cannot create an observation");

constexpr ScoreBand fastBand{0.25, 0.75};
constexpr ScoreBand preciseBand{0.125, 0.875};
constexpr auto budget = 10000us;

struct Case {
    const char* name;
    AnalysisMeasurement fast;
    AnalysisMeasurement precise;
    Outcome outcome;
    Reason reason;
    int expectedCalls;
};

void check_cases()
{
    const SelectiveInspectionPolicy policy(fastBand, preciseBand, budget);
    const AnalysisMeasurement uncertainFast{0.5, 2000us};
    const AnalysisMeasurement normalPrecise{0.125, 3000us};
    constexpr AnalysisMeasurement unusedPrecise{std::nullopt, 0us};
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const Case cases[]{
        {"Fast below PASS boundary", {0.0, 2000us}, unusedPrecise, Outcome::Pass, Reason::FastThreshold, 0},
        {"Fast above FAIL boundary", {1.0, 2000us}, unusedPrecise, Outcome::Fail, Reason::FastThreshold, 0},
        {"Precise below PASS boundary", uncertainFast, {0.0, 3000us}, Outcome::Pass, Reason::PreciseThreshold, 1},
        {"Precise above FAIL boundary", uncertainFast, {1.0, 3000us}, Outcome::Fail, Reason::PreciseThreshold, 1},
        {"Precise PASS returned late", uncertainFast, {0.125, 8001us}, Outcome::Review, Reason::BudgetExhausted, 1},
        {"Fast just above PASS", {std::nextafter(0.25, 1.0), 0us}, normalPrecise,
            Outcome::Pass, Reason::PreciseThreshold, 1},
        {"Fast just below FAIL", {std::nextafter(0.75, 0.0), 0us}, normalPrecise,
            Outcome::Pass, Reason::PreciseThreshold, 1},
        {"Precise just above PASS", uncertainFast, {std::nextafter(0.125, 1.0), 0us},
            Outcome::Review, Reason::PreciseUncertain, 1},
        {"Precise just below FAIL", uncertainFast, {std::nextafter(0.875, 0.0), 0us},
            Outcome::Review, Reason::PreciseUncertain, 1},
        {"Fast PASS consumes budget", {0.25, budget}, unusedPrecise, Outcome::Pass, Reason::FastThreshold, 0},
        {"Fast FAIL consumes budget", {0.75, budget}, unusedPrecise, Outcome::Fail, Reason::FastThreshold, 0},
        {"Fast decisive result returned late", {0.75, budget + 1us}, unusedPrecise, Outcome::Review, Reason::BudgetExhausted, 0},
        {"Fast failure", {std::nullopt, budget + 1us}, unusedPrecise, Outcome::Error, Reason::FastAnalysisError, 0},
        {"Late precise failure", uncertainFast, {std::nullopt, 8001us}, Outcome::Error, Reason::PreciseAnalysisError, 1},
        {"Fast score out of bounds", {-0.01, 1us}, unusedPrecise, Outcome::Error, Reason::InvalidFastMeasurement, 0},
        {"Fast score NaN", {nan, budget + 1us}, unusedPrecise, Outcome::Error, Reason::InvalidFastMeasurement, 0},
        {"Fast negative time", {0.5, -1us}, unusedPrecise, Outcome::Error, Reason::InvalidFastMeasurement, 0},
        {"Precise score NaN", uncertainFast, {nan, 3000us}, Outcome::Error, Reason::InvalidPreciseMeasurement, 1},
        {"Precise score infinite", uncertainFast, {infinity, 8001us}, Outcome::Error, Reason::InvalidPreciseMeasurement, 1},
        {"Precise score out of bounds", uncertainFast, {1.01, 1us}, Outcome::Error, Reason::InvalidPreciseMeasurement, 1},
        {"Precise negative time", uncertainFast, {0.125, -1us}, Outcome::Error, Reason::InvalidPreciseMeasurement, 1},
        {"Large precise duration", uncertainFast, {0.125, std::chrono::microseconds::max()},
            Outcome::Review, Reason::BudgetExhausted, 1},
    };
    for (const auto& sample : cases) {
        int calls = 0;
        const auto result = policy.evaluate(sample.fast, [&] {
            ++calls;
            return sample.precise;
        });
        if (result.outcome != sample.outcome || result.reason != sample.reason
            || calls != sample.expectedCalls || result.precise.has_value() != (calls == 1)) {
            throw std::runtime_error(std::string(sample.name) + ": incorrect outcome, reason or call count");
        }
        if (result.precise) {
            const auto actual = result.precise->score;
            const auto expected = sample.precise.score;
            const bool sameScore = actual == expected
                || (actual && expected && std::isnan(*actual) && std::isnan(*expected));
            if (!sameScore || result.precise->elapsed != sample.precise.elapsed)
                throw std::runtime_error(std::string(sample.name) + ": precise observation was not preserved");
        }
    }
    const auto missingCallback = policy.evaluate(uncertainFast, {});
    if (missingCallback.outcome != Outcome::Error || missingCallback.reason != Reason::PreciseAnalysisError
        || missingCallback.precise)
        throw std::runtime_error("Missing required precise callback must report ERROR without an attempt");
}

void check_invalid_config()
{
    const auto rejects = [](ScoreBand fast, ScoreBand precise, std::chrono::microseconds timeBudget) {
        try {
            static_cast<void>(SelectiveInspectionPolicy(fast, precise, timeBudget));
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error("Invalid selective inspection configuration accepted");
    };
    rejects({0.5, 0.5}, preciseBand, budget);
    rejects(fastBand, {0.9, 0.8}, budget);
    rejects({-0.01, 0.75}, preciseBand, budget);
    rejects(fastBand, {0.125, 1.01}, budget);
    rejects({std::numeric_limits<double>::quiet_NaN(), 0.75}, preciseBand, budget);
    rejects(fastBand, {0.125, std::numeric_limits<double>::infinity()}, budget);
    rejects(fastBand, preciseBand, 0us);
    rejects(fastBand, preciseBand, -1us);
}
}

int main()
{
    try {
        check_invalid_config();
        check_cases();
        std::cout << "Selective inspection policy checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
