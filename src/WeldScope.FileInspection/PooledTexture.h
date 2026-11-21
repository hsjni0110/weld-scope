#pragma once
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace pooled_texture {
// 학습과 실행에서 동일하게 사용하는 영상 격자와 특징 수.
constexpr int gridColumns = 8, gridRows = 4, channels = 14;
constexpr int featureCount = (gridColumns * gridRows + gridRows + 1) * channels;
// 모델에 전달하는 고정 순서의 영상 질감 특징.
using Features = std::array<float, featureCount>;

// 비어 있지 않은 8비트 회색 또는 BGR 영상에서 모델의 입력 특징을 만든다.
// 원본 픽셀 간격을 사용해 좁은 홈과 넓은 요철의 밝기 차를 함께 반영한다.
inline Features describe(const cv::Mat& image)
{
    cv::Mat gray;
    if (image.type() == CV_8UC1) gray = image;
    else if (image.type() == CV_8UC3) cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    else throw std::invalid_argument("Expected 8-bit grayscale or BGR image");
    if (gray.empty()) throw std::invalid_argument("Empty image");
    constexpr int samplesX = gridColumns * 8, samplesY = gridRows * 4;
    std::array<std::array<int, 5>, samplesX> xs{};
    for (int x = 0; x < samplesX; ++x) {
        const int mid = static_cast<int>((2LL*x+1)*gray.cols/(2*samplesX));
        xs[x] = {std::max(0,mid-4),std::max(0,mid-1),mid,std::min(gray.cols-1,mid+1),std::min(gray.cols-1,mid+4)};
    }
    struct Cell {
        std::array<int, channels> sum{};
        int low = 255, high = 0;
    };
    std::array<Cell, gridColumns*gridRows> cells{};
    for (int y = 0; y < samplesY; ++y) {
        const int center = static_cast<int>((2LL*y+1)*gray.rows/(2*samplesY));
        const auto mid = gray.ptr<uchar>(center);
        const auto top = gray.ptr<uchar>(std::max(0,center-1)), bottom = gray.ptr<uchar>(std::min(gray.rows-1,center+1));
        const auto farTop = gray.ptr<uchar>(std::max(0,center-4)), farBottom = gray.ptr<uchar>(std::min(gray.rows-1,center+4));
        for (int x = 0; x < samplesX; ++x) {
            const auto& col = xs[x];
            const int i = col[2], value = mid[i];
            auto& cell = cells[(y/4)*gridColumns+x/8];
            auto& sum = cell.sum;
            sum[0] += value;
            sum[1] += value*value;
            cell.low = std::min(cell.low,value);
            cell.high = std::max(cell.high,value);
            sum[4] += std::abs(mid[col[3]]-mid[col[1]]);
            sum[5] += std::abs(bottom[i]-top[i]);
            sum[6] += std::abs(mid[col[4]]-mid[col[0]]);
            sum[7] += std::abs(farBottom[i]-farTop[i]);
            const int horizontal = mid[col[4]]+mid[col[0]]-2*value;
            const int vertical = farBottom[i]+farTop[i]-2*value;
            sum[8] += std::max(0,horizontal);
            sum[9] += std::max(0,vertical);
            sum[10] += std::max(0,-horizontal);
            sum[11] += std::max(0,-vertical);
            sum[12] += std::abs(mid[col[1]]+mid[col[3]]-2*value);
            sum[13] += std::abs(top[i]+bottom[i]-2*value);
        }
    }
    Features result{};
    constexpr size_t rowStart = gridRows*gridColumns*channels;
    constexpr size_t globalStart = rowStart+gridRows*channels;
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& c = cells[i];
        for (size_t k = 0; k < channels; ++k) {
            float v = c.sum[k]/32.f;
            if (k == 1) v = std::sqrt(std::max(0.f, v - result[i*channels]*result[i*channels]));
            if (k == 2) v = static_cast<float>(c.low);
            if (k == 3) v = static_cast<float>(c.high);
            result[i*channels+k] = v;
            result[rowStart+(i/gridColumns)*channels+k] += v/gridColumns;
            result[globalStart+k] += v/(gridRows*gridColumns);
        }
    }
    return result;
}
}
