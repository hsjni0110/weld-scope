#pragma once

#include "SelectiveInspectionPolicy.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <vector>

namespace weldscope {

// 단일 분석이나, 점수에 따라 두 번째 분석을 선택하는 검사 경로를 지정한다.
enum class InspectionMode { Baseline, Fast, Precise, Selective, PooledSmooth };
// 학습 모델을 사용하는 배포 모드의 고정 설정 파일 이름.
inline constexpr auto pooledSmoothConfigFile = L"pooled-smooth-220.json";
class PooledSmoothModel;

// 단일 분석은 failRatio를 사용한다. 선택 경로는 두 점수 구간과 시간 한도를 사용한다.
struct InspectionParameters {
    InspectionMode mode;
    int threshold;
    double failRatio;
    ScoreBand fastBand;
    ScoreBand preciseBand;
    std::chrono::microseconds budget;
};

// 실제로 실행한 한 단계의 점수와 소요 시간. 이 단계의 판정은 최종 판정과 다를 수 있다.
struct InspectionAttempt {
    std::string stage;
    std::optional<double> score;
    InspectionOutcome outcome;
    std::string reason;
    std::chrono::nanoseconds elapsed;
};

// 최종 판정과 실행 순서대로 기록한 분석 시도를 제공한다. 후보 영상이 없는 경로에서는 마스크가 비어 있다.
struct AnalysisOutput {
    cv::Mat gray, mask;
    std::uint64_t candidatePixels = 0, totalPixels = 0;
    std::optional<double> score, continuousCandidateRatio;
    InspectionOutcome outcome = InspectionOutcome::Error;
    std::string reason;
    double analysisMs = 0;
    std::vector<InspectionAttempt> attempts;
};

// 검사 경로에 필요한 필드를 읽고 범위를 검증한다. 올바르지 않은 설정은 예외로 알린다.
InspectionParameters parse_inspection_parameters(const cv::FileStorage& json, InspectionMode mode);
// 이력에 기록할 검사 알고리즘의 고정 버전 문자열을 돌려준다.
const char* inspection_version(InspectionMode mode);
// DB와 장비 통신에 공통으로 쓰는 영문 판정 코드를 돌려준다.
const char* outcome_name(InspectionOutcome outcome);
// 8비트 BGR 영상의 판정과 근거를 계산한다. 모델은 PooledSmooth 경로에서만 필요하다.
// beforePrecise는 두 번째 분석을 시작할 때만 호출한다. 모델은 호출이 끝날 때까지 유효해야 한다.
AnalysisOutput analyze_image(const cv::Mat& bgr, const InspectionParameters& parameters,
    const std::function<void()>& beforePrecise = {}, const PooledSmoothModel* model = nullptr);

}
