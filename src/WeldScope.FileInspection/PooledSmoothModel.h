#pragma once

#include "CompactBoostedTrees.h"
#include "InspectionInput.h"
#include "InterpolatedScoreBand.h"

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace weldscope {

// 배포용으로 고정한 학습 모델을 불러오고 영상 점수와 폭에 맞는 판정 경계를 제공한다.
class PooledSmoothModel {
public:
    // 등록된 설정 바이트를 식별하는 SHA-256 값.
    static constexpr auto configHash = "6b85803c90e4dfa5d58ed7f181a91d5b0f3176d92522272c5cd85d0cbd9f2955";

    // 설정과 모델 파일의 지문을 모두 확인한다. 일치하지 않으면 std::invalid_argument를 던진다.
    PooledSmoothModel(const std::filesystem::path& directory, const std::vector<unsigned char>& manifest)
        : PooledSmoothModel(directory, checked_settings(manifest)) {}

    // 영상의 가로 픽셀 수로 통과와 불량의 경계를 구한다.
    ScoreBand band(int width) const {
        return pooled_texture::interpolated_band(width, inside_, outside_);
    }
    // 8비트 영상의 질감 특징을 추출해 모델 점수를 구한다. 판정은 호출자가 경계와 비교해 내린다.
    double score(const cv::Mat& bgr) const {
        return trees_.predict(pooled_texture::describe(bgr));
    }

private:
    static cv::FileStorage checked_settings(const std::vector<unsigned char>& manifest) {
        if (sha256(manifest) != configHash) throw std::invalid_argument("Frozen manifest hash mismatch");
        auto settings = read_json(manifest);
        if (read_integer(settings["schema_version"], "schema_version") != 2)
            throw std::invalid_argument("Unsupported LightGBM settings");
        return settings;
    }
    static pooled_texture::CompactBoostedTrees load_trees(const std::filesystem::path& directory,
            const cv::FileStorage& settings) {
        const auto bytes = read_bytes(directory / std::filesystem::u8path(settings["model"].string()), 16 * 1024 * 1024);
        if (sha256(bytes) != settings["model_sha256"].string()) throw std::invalid_argument("Frozen model hash mismatch");
        std::istringstream stream(std::string(bytes.begin(), bytes.end()));
        return pooled_texture::CompactBoostedTrees(stream);
    }
    PooledSmoothModel(const std::filesystem::path& directory, const cv::FileStorage& settings)
        : inside_{settings["inside_pass"].real(), settings["inside_fail"].real()},
          outside_{settings["outside_pass"].real(), settings["outside_fail"].real()},
          trees_(load_trees(directory, settings)) {}

    ScoreBand inside_, outside_;
    pooled_texture::CompactBoostedTrees trees_;
};
}
