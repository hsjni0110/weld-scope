#pragma once
#include <opencv2/core.hpp>
#include <cstdint>
#include <optional>

namespace weldscope {
// Fast는 가로 9픽셀의 밝기 차를, Precise는 더 넓은 비교 범위와 가로 연결성도 반영한다.
enum class LocalContrastMode { Fast, Precise };

// 마스크와 후보 수는 표시용 후보 전체를 담는다. 연속 후보 비율은 Precise에서만 제공한다.
struct LocalContrastResult {
    cv::Mat gray;
    cv::Mat mask;
    std::uint64_t candidatePixels;
    std::uint64_t totalPixels;
    double score;
    bool isFail;
    std::optional<double> continuousCandidateRatio;
};

// 밝기 차 기준 0~255와 불량 비율 0~1을 검사한다. 유효하지 않으면 std::invalid_argument를 던진다.
void validate_local_contrast_config(int contrastThreshold, double failRatio);

// 2차원 8비트 BGR 영상을 분석한다. 점수가 failRatio 이상이면 불량이며, 빈 영상은 받지 않는다.
LocalContrastResult inspect_local_contrast(const cv::Mat& bgr, LocalContrastMode mode,
    int contrastThreshold, double failRatio);
}
