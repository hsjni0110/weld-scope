#pragma once
#include "../openzdm_boost/PooledTexture.h"
#include <cstdint>
#include <limits>

namespace pooled_texture {
class CompactBoostedTrees {
    // A split's two children are adjacent. Indexes are relative to this tree.
    struct Node { float threshold; int16_t feature, left; };
    static_assert(sizeof(Node)==8);
    struct Tree { size_t root, leafStart; };
    std::vector<Node> nodes_;
    std::vector<Tree> trees_;
    std::vector<double> leaves_;

public:
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
                // Features are float. Rounding the cut down preserves comparison
                // with the original double threshold, including exact ties.
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
