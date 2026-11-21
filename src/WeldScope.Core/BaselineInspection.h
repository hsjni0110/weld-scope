#pragma once
#include <opencv2/core.hpp>
#include <cstdint>

namespace weldscope {
// 어두운 영역의 비율을 기준으로 내린 이미지 한 장의 통과 또는 불량 판정.
enum class BaselineDecision { Pass, Fail };

// 기준 검사에 사용할 밝기 경계와 불량 비율을 검증해 보관한다.
class BaselineConfig {
public:
    // 밝기 경계는 0~255, 불량 비율은 0~1이다. 범위를 벗어나면 std::invalid_argument를 던진다.
    BaselineConfig(int intensityThreshold, double failRatio);
    // 이 값 이하의 회색 픽셀을 후보로 센다.
    int intensityThreshold() const { return intensityThreshold_; }
    // 후보 비율이 이 값 이상이면 불량으로 판정한다.
    double failRatio() const { return failRatio_; }
private:
    int intensityThreshold_;
    double failRatio_;
};

// 원본 크기의 회색 영상과 이진 마스크, 후보 비율 및 판정을 함께 전달한다.
struct BaselineResult {
    cv::Mat gray;
    cv::Mat mask;
    std::uint64_t candidatePixels;
    std::uint64_t totalPixels;
    double score;
    BaselineDecision decision;
};

// 비어 있지 않은 2차원 8비트 BGR 영상 한 장을 검사한다. 형식이 다르면 std::invalid_argument를 던진다.
BaselineResult inspect_baseline(const cv::Mat& bgr, const BaselineConfig& config);
}
