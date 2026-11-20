#include "SelectiveInspectionPolicy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace weldscope;
using std::chrono::microseconds;
using std::chrono::nanoseconds;

struct Observation {
    bool isKo;
    AnalysisMeasurement fast;
    AnalysisMeasurement precise;
};

struct Candidate {
    ScoreBand fast{};
    ScoreBand precise{};
    std::array<std::array<int, 4>, 2> counts{};
    int preciseCount = 0;
    int koExcess = 0;
    int automaticShortfall = 0;
    int preciseExcess = 0;

    int koPass() const { return counts[1][0]; }
    int okFail() const { return counts[0][1]; }
    int review() const { return counts[0][2] + counts[1][2]; }
    int automatic() const { return counts[0][0] + counts[0][1] + counts[1][0] + counts[1][1]; }
    int deficit() const { return koExcess + automaticShortfall + preciseExcess; }
    auto rank() const {
        return std::make_tuple(deficit(), okFail(), koPass(), preciseCount, review(),
                               fast.passMax, fast.failMin, precise.passMax, precise.failMin);
    }
};

std::optional<double> observed_score(double score) {
    if (score == -1.0) return std::nullopt;
    if (!std::isfinite(score) || score < 0 || score > 1)
        throw std::invalid_argument("Stage score outside [0,1]");
    return score;
}

std::vector<double> boundaries(std::vector<double> scores) {
    std::vector<double> values{0, 1};
    if (scores.empty()) return values;
    std::sort(scores.begin(), scores.end());
    for (std::size_t percentile = 0; percentile <= 10; ++percentile) {
        const auto rank = (percentile * scores.size() + 9) / 10;
        values.push_back(scores[rank ? rank - 1 : 0]);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

int column(InspectionOutcome outcome) {
    switch (outcome) {
    case InspectionOutcome::Pass: return 0;
    case InspectionOutcome::Fail: return 1;
    case InspectionOutcome::Review: return 2;
    case InspectionOutcome::Error: return 3;
    }
    throw std::logic_error("Unknown inspection outcome");
}

void write_candidate(const Candidate& candidate) {
    std::cout << "{\"fast_pass_max\":" << candidate.fast.passMax << ",\"fast_fail_min\":" << candidate.fast.failMin
        << ",\"precise_pass_max\":" << candidate.precise.passMax << ",\"precise_fail_min\":" << candidate.precise.failMin
        << ",\"eligible\":" << (candidate.deficit() == 0 ? "true" : "false")
        << ",\"precise_count\":" << candidate.preciseCount << ",\"automatic\":" << candidate.automatic()
        << ",\"deficits\":{\"ko_excess\":" << candidate.koExcess
        << ",\"automatic_shortfall\":" << candidate.automaticShortfall
        << ",\"precise_excess\":" << candidate.preciseExcess << "},\"counts\":[";
    for (std::size_t label = 0; label < candidate.counts.size(); ++label) {
        if (label) std::cout << ',';
        const auto& count = candidate.counts[label];
        std::cout << '[' << count[0] << ',' << count[1] << ',' << count[2] << ',' << count[3] << ']';
    }
    std::cout << "]}";
}

void calibrate() {
    std::string observationUnit;
    std::int64_t budget = 0;
    int count = 0, maximumKo = 0, minimumAutomaticPercent = 0, maximumPrecisePercent = 0;
    if (!(std::cin >> observationUnit >> count >> budget >> maximumKo >> minimumAutomaticPercent >> maximumPrecisePercent) ||
        observationUnit != "nanoseconds" ||
        count <= 0 || count > 100000 || budget <= 0 || maximumKo < 0 || maximumKo > count ||
        minimumAutomaticPercent < 0 || minimumAutomaticPercent > 100 ||
        maximumPrecisePercent < 0 || maximumPrecisePercent > 100)
        throw std::invalid_argument("Invalid validation header");
    const int minimumAutomatic = (count * minimumAutomaticPercent + 99) / 100;
    const int maximumPrecise = count * maximumPrecisePercent / 100;
    std::vector<Observation> observations;
    std::vector<double> fastScores, preciseScores;
    observations.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        int label = -1;
        double fastScore = 0, preciseScore = 0;
        std::int64_t fastNs = 0, preciseNs = 0;
        if (!(std::cin >> label >> fastScore >> fastNs >> preciseScore >> preciseNs) ||
            (label != 0 && label != 1) || fastNs < 0 || preciseNs < 0)
            throw std::invalid_argument("Invalid validation observation");
        auto fast = observed_score(fastScore);
        auto precise = observed_score(preciseScore);
        if (fast) fastScores.push_back(*fast);
        if (precise) preciseScores.push_back(*precise);
        observations.push_back({label == 1, AnalysisMeasurement::from_nanoseconds(fast, nanoseconds(fastNs)),
            AnalysisMeasurement::from_nanoseconds(precise, nanoseconds(preciseNs))});
    }
    std::cin >> std::ws;
    if (!std::cin.eof()) throw std::invalid_argument("Unexpected trailing observations");
    const auto fast = boundaries(std::move(fastScores));
    const auto precise = boundaries(std::move(preciseScores));
    std::optional<Candidate> selected;
    int eligible = 0;
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10)
              << "{\"count_order\":[\"PASS\",\"FAIL\",\"REVIEW\",\"ERROR\"],\"label_order\":[\"OK\",\"KO\"],\"candidates\":[";
    for (std::size_t fp = 0; fp + 1 < fast.size(); ++fp)
        for (std::size_t ff = fp + 1; ff < fast.size(); ++ff)
            for (std::size_t pp = 0; pp + 1 < precise.size(); ++pp)
                for (std::size_t pf = pp + 1; pf < precise.size(); ++pf) {
                    Candidate candidate{{fast[fp], fast[ff]}, {precise[pp], precise[pf]}};
                    const SelectiveInspectionPolicy policy(candidate.fast, candidate.precise, microseconds(budget));
                    for (const auto& row : observations) {
                        const auto result = policy.evaluate(row.fast, [&row] { return row.precise; });
                        ++candidate.counts[row.isKo ? 1 : 0][column(result.outcome)];
                        if (result.precise) ++candidate.preciseCount;
                    }
                    candidate.koExcess = std::max(0, candidate.koPass() - maximumKo);
                    candidate.automaticShortfall = std::max(0, minimumAutomatic - candidate.automatic());
                    candidate.preciseExcess = std::max(0, candidate.preciseCount - maximumPrecise);
                    if (candidate.deficit() == 0) ++eligible;
                    if (selected) std::cout << ',';
                    write_candidate(candidate);
                    if (!selected || candidate.rank() < selected->rank()) selected = candidate;
                }
    std::cout << "],\"eligible_candidates\":" << eligible << ",\"selected\":";
    write_candidate(selected.value());
    std::cout << "}\n";
}
}

int main() {
    try {
        calibrate();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
