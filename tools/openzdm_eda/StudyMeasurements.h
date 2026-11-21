#pragma once
#include "AnalysisPipeline.h"
#include "LocalContrastInspection.h"

#include <opencv2/imgproc.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>

namespace study {
using namespace weldscope;
using Clock = std::chrono::steady_clock;
using namespace std::chrono;
namespace fs = std::filesystem;

inline const char* reason_name(PolicyReason reason)
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
    throw std::logic_error("Unknown policy reason");
}

inline AnalysisMeasurement observe(const cv::Mat& image, int threshold, LocalContrastMode mode, bool center = false)
{
    const auto start = Clock::now();
    std::optional<double> score;
    try {
        const auto result = inspect_local_contrast(image, mode, threshold, 0);
        if (center) {
            const int top = image.rows / 4;
            const cv::Rect band(2, top, image.cols - 4, image.rows - 2 * top);
            score = cv::countNonZero(result.mask(band)) / static_cast<double>(result.mask(band).total());
        } else {
            score = result.score;
        }
    } catch (const cv::Exception&) {
    } catch (const std::invalid_argument&) {}
    return AnalysisMeasurement::from_nanoseconds(score, duration_cast<nanoseconds>(Clock::now() - start));
}

inline void write_measurement(std::ostream& output, const AnalysisMeasurement& measurement)
{
    if (measurement.score) output << *measurement.score;
    output << ',' << (duration_cast<nanoseconds>(measurement.elapsed) + measurement.fraction()).count();
}
}
