#pragma once

#include "AnalysisPipeline.h"
#include "../../tools/openzdm_boost_guard/CompactBoostedTrees.h"
#include "../../tools/openzdm_boost_smooth/InterpolatedScoreBand.h"
#include <sstream>

namespace weldscope {

class PooledSmoothModel {
public:
    static constexpr auto configHash = "6a557520c10a26ec9ce7dc9585916c57b9dab34a421bc6650bf3d2227dfa7d7b";

    PooledSmoothModel(const std::filesystem::path& directory, const std::vector<unsigned char>& manifest)
        : PooledSmoothModel(directory, checked_settings(manifest)) {}

    ScoreBand band(int width) const {
        return pooled_texture::interpolated_band(width, inside_, outside_);
    }
    double score(const cv::Mat& bgr) const {
        return trees_.predict(pooled_texture::describe(bgr));
    }

private:
    static cv::FileStorage checked_settings(const std::vector<unsigned char>& manifest) {
        if (sha256(manifest) != configHash) throw std::invalid_argument("Frozen manifest hash mismatch");
        return read_json(manifest);
    }
    static pooled_texture::CompactBoostedTrees load_trees(const std::filesystem::path& directory,
            const cv::FileStorage& settings) {
        const auto bytes = read_bytes(directory / std::filesystem::u8path(settings["model"].string()), 16 * 1024 * 1024);
        if (sha256(bytes) != settings["model_sha256"].string()) throw std::invalid_argument("Frozen model hash mismatch");
        std::istringstream stream(std::string(bytes.begin(), bytes.end()));
        return pooled_texture::CompactBoostedTrees(stream);
    }
    PooledSmoothModel(const std::filesystem::path& directory, const cv::FileStorage& settings)
        : inside_{settings["fast_pass_max"].real(), settings["fast_fail_min"].real()},
          outside_{settings["outside_pass"].real(), settings["outside_fail"].real()},
          trees_(load_trees(directory, settings)) {}

    ScoreBand inside_, outside_;
    pooled_texture::CompactBoostedTrees trees_;
};
}
