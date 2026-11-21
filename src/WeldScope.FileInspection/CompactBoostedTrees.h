#pragma once
#include "PooledTexture.h"
#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace pooled_texture {
// 같은 특징과 모델 형식을 사용하면서 트리 탐색용 노드를 연속된 배열에 보관한다.
class CompactBoostedTrees {
    // 분기 노드의 두 자식은 인접해 있다. 번호는 해당 트리의 시작 위치를 기준으로 한다.
    struct Node { float threshold; int16_t feature, left; };
    static_assert(sizeof(Node)==8);
    struct Tree { size_t root, leafStart; };
    std::vector<Node> nodes_;
    std::vector<Tree> trees_;
    std::vector<double> leaves_;

public:
    // 호출자가 연 스트림에서 모델을 읽고 검증한다. 읽기 오류가 나면 스트림이 예외를 던지도록 설정한다.
    explicit CompactBoostedTrees(std::istream& input)
    {
        input.exceptions(std::ios::badbit | std::ios::failbit);
        std::string version;
        int columns, count;
        input >> version >> columns >> count;
        if (version != "pooled-texture-v1" || columns != featureCount || count < 1 || count > 4096)
            throw std::invalid_argument("Invalid pooled texture model header");
        struct SourceNode { float threshold; int16_t feature, left, right; };
        for (int t=0; t<count; ++t) {
            int leaves;
            input >> leaves;
            if (leaves < 2 || leaves > 1024) throw std::invalid_argument("Invalid leaf count");
            std::vector<SourceNode> source;
            for (int i=0; i<leaves-1; ++i) {
                int feature, left, right;
                double threshold;
                input >> feature >> threshold >> left >> right;
                const auto valid = [=](int child) { return child >= -leaves && (child < 0 || (child > i && child < leaves-1)); };
                if (feature < 0 || feature >= featureCount || !std::isfinite(threshold) || !valid(left) || !valid(right))
                    throw std::invalid_argument("Invalid tree node");
                // float인 특징을 원래 double 경계와 비교했을 때와 같은 결과가 나도록 경계를 아래로 맞춘다.
                float cut = static_cast<float>(threshold);
                if (cut > threshold) cut = std::nextafter(cut,-std::numeric_limits<float>::infinity());
                source.push_back({cut,static_cast<int16_t>(feature),static_cast<int16_t>(left),static_cast<int16_t>(right)});
            }
            const Tree tree{nodes_.size(),leaves_.size()};
            trees_.push_back(tree);
            for (int i=0; i<leaves; ++i) {
                double value;
                input >> value;
                if (!std::isfinite(value)) throw std::invalid_argument("Invalid tree leaf");
                leaves_.push_back(value);
            }
            std::vector<int16_t> original{0};
            for (size_t i=0; i<original.size(); ++i) {
                const int old = original[i];
                if (old < 0) nodes_.push_back({0,-1,static_cast<int16_t>(-1-old)});
                else {
                    if (original.size()+2 > static_cast<size_t>(2*leaves-1))
                        throw std::invalid_argument("Invalid repeated tree node");
                    const auto& node = source[old];
                    nodes_.push_back({node.threshold,node.feature,static_cast<int16_t>(original.size())});
                    original.push_back(node.left);
                    original.push_back(node.right);
                }
            }
            if (original.size() != static_cast<size_t>(2*leaves-1)) throw std::invalid_argument("Unreachable tree nodes");
        }
    }
    // describe와 같은 순서의 특징을 받아, 트리 출력의 합을 0~1 점수로 바꾼다.
    double predict(const Features& features) const
    {
        double raw = 0;
        for (const auto& tree : trees_) {
            const auto* root = nodes_.data()+tree.root;
            const auto* node = root;
            while (node->feature >= 0)
                node = root+node->left+(features[node->feature] > node->threshold);
            raw += leaves_[tree.leafStart+node->left];
        }
        return 1/(1+std::exp(-raw));
    }
};
}
