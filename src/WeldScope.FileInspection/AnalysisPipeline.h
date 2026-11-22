#pragma once

#include <chrono>
#include <opencv2/core.hpp>
#include <optional>

namespace weldscope {

enum class InspectionOutcome { Pass, Fail, Review, Error };

struct ScoreBand {
    double passMax;
    double failMin;
};

inline constexpr auto pooledSmoothConfigFile = L"pooled-smooth-220.json";
inline constexpr auto inspectionConfigId = "pooled-smooth-220-v2";
inline constexpr auto inspectionVersion = "openzdm-pooled-smooth-220-v2";
inline constexpr auto inspectionStage = "LIGHTGBM";
class PooledSmoothModel;

struct AnalysisOutput {
    std::optional<double> score;
    InspectionOutcome outcome;
    const char* reason;
    std::chrono::nanoseconds elapsed;
};

// 두 판정선에 닿으면 정상 또는 불량이며, 그 사이의 점수는 검토 대상으로 남긴다.
InspectionOutcome classify_score(double score, ScoreBand band);
// DB와 장비 통신에 공통으로 쓰는 영문 판정 코드를 반환한다.
const char* outcome_name(InspectionOutcome outcome);
// 사진 한 장의 LightGBM 점수를 한 번 계산한다. 경과 시간은 기록용이다.
AnalysisOutput analyze_image(const cv::Mat& bgr, const PooledSmoothModel& model);

}
