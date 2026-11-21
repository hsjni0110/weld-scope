#pragma once

#include "PooledTexture.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pooled_texture {
// 학습한 트리 묶음을 파일에서 읽어 영상 특징의 점수를 계산하는 평가용 객체.
class BoostedTrees {
    struct Node { int feature, left, right; double threshold; };
    struct Tree { std::vector<Node> nodes; std::vector<double> leaves; };
    std::vector<Tree> trees_;
public:
    // pooled-texture-v1 형식과 특징 수를 확인한다. 읽기 오류나 모델 형식 오류는 예외로 알린다.
    explicit BoostedTrees(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        input.exceptions(std::ios::failbit | std::ios::badbit);
        std::string version;
        int features, count;
        input >> version >> features >> count;
        if (version != "pooled-texture-v1" || features != featureCount || count < 1 || count > 4096)
            throw std::invalid_argument("Invalid pooled texture model header");
        trees_.resize(count);
        for (auto& tree : trees_) {
            int leaves;
            input >> leaves;
            if (leaves < 2 || leaves > 1024) throw std::invalid_argument("Invalid leaf count");
            tree.nodes.resize(leaves-1);
            tree.leaves.resize(leaves);
            for (int i = 0; i < leaves-1; ++i) {
                auto& node = tree.nodes[i];
                input >> node.feature >> node.threshold >> node.left >> node.right;
                const auto valid = [=](int child) { return child >= -leaves && (child < 0 || (child > i && child < leaves-1)); };
                if (node.feature < 0 || node.feature >= featureCount || !std::isfinite(node.threshold) ||
                    !valid(node.left) || !valid(node.right)) throw std::invalid_argument("Invalid tree node");
            }
            for (auto& leaf : tree.leaves) {
                input >> leaf;
                if (!std::isfinite(leaf)) throw std::invalid_argument("Invalid tree leaf");
            }
        }
    }
    // describe가 만든 순서대로 특징을 받는다. 트리 출력의 합을 0~1 점수로 바꾼다.
    double predict(const Features& features) const
    {
        double raw = 0;
        for (const auto& tree : trees_) {
            int index = 0;
            while (index >= 0) {
                const auto& node = tree.nodes[index];
                index = features[node.feature] <= node.threshold ? node.left : node.right;
            }
            raw += tree.leaves[static_cast<size_t>(-1-index)];
        }
        return 1/(1+std::exp(-raw));
    }
};
}
