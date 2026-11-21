#pragma once

#include <opencv2/core.hpp>
#include <array>
#include <filesystem>
#include <vector>

namespace pattern_study {
// 같은 입력을 비교할 세 가지 특징 수. 모델 배열도 이 순서를 따른다.
inline constexpr std::array<int, 3> lengths{32, 96, 192};
// 실험에서 추출한 최대 192개의 특징. 짧은 모델은 앞부분만 사용한다.
using Features = std::array<double, 192>;
// 특징을 정규화한 뒤 가중합으로 점수를 내는 실험 모델.
struct Model {
    std::vector<double> mean, scale, weights;
    double intercept = 0;
    // 해당 특징 수로 추출한 값을 받아 0~1 점수를 반환한다.
    double score(const Features& values) const;
};
// 8비트 회색 또는 BGR 영상에서 지정한 특징 수를 추출한다. length는 lengths의 값 중 하나다.
Features extract(const cv::Mat& image, int length);
// 설정에서 세 모델을 특징 수 순서로 읽는다. 계수와 정규화 값의 개수가 맞지 않으면 예외를 던진다.
std::array<Model, 3> load(const cv::FileStorage& file);
// 비어 있지 않은 영상별 특징과 같은 길이의 0 또는 1 라벨로 학습하고 결과를 JSON에 쓴다.
// checkConvergence를 켜면 수렴을 비교할 수 있는 진단 결과도 기록한다.
void train(const std::vector<Features>& rows, const std::vector<int>& labels,
           const std::filesystem::path& destination, bool checkConvergence);
}
