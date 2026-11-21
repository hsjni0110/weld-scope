#pragma once

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace weldscope::spatial {
// 가로 10픽셀, 세로 16픽셀 이상인 8비트 회색 영상의 구역별 질감 특징을 result에 추가한다.
// outer면 바깥쪽을 쓰며, removeOrder면 위치 순서의 영향을 비교할 수 있게 구역을 정렬한다.
inline void tile_features(const cv::Mat& gray, bool outer, bool removeOrder, std::vector<float>& result)
{
    if (gray.type()!=CV_8UC1 || gray.rows<16 || gray.cols<10)
        throw std::invalid_argument("Spatial descriptor needs at least 10 by 16 grayscale pixels");
    constexpr int channels=8;
    // Sampled patches in this experiment are small; use 64-bit sums for any supported height.
    struct Cell { int n=0; int64_t sum=0, square=0; std::array<int64_t,6> edge{}; };
    std::array<Cell,32> cells{};
    const int q=gray.rows/4;
    double sum=0, square=0;
    int total=0;
    for (int band=0; band<2; ++band) {
        const int start=outer?(band?gray.rows-q:0):(band?gray.rows-2*q:q);
        for (int sy=0; sy<2; ++sy) for (int sx=0; sx<8; ++sx) {
            auto& c=cells[(band*2+sy)*8+sx];
            const int y0=start+1+(q-2)*sy/2, y1=start+1+(q-2)*(sy+1)/2;
            for (int y=y0;y<y1;++y) {
                const auto *mid=gray.ptr<uchar>(y), *up=gray.ptr<uchar>(y-1), *down=gray.ptr<uchar>(y+1);
                const int right=1+(gray.cols-2)*(sx+1)/8;
                for (int x=1+(gray.cols-2)*sx/8;x<right;x+=2) {
                    const int v=mid[x], dx=int(mid[x+1])-mid[x-1], dy=int(down[x])-up[x];
                    ++c.n; c.sum+=v; c.square+=v*v;
                    c.edge[0]+=std::abs(dx); c.edge[1]+=std::abs(dy);
                    c.edge[2]+=std::abs(2*v-mid[x+1]-mid[x-1]);
                    c.edge[3]+=std::abs(2*v-up[x]-down[x]);
                    c.edge[4]+=std::max(0,std::min(v-int(mid[x+1]),v-int(mid[x-1])));
                    c.edge[5]+=std::max(0,std::min(int(up[x])-v,int(down[x])-v));
                }
            }
            total+=c.n; sum+=static_cast<double>(c.sum); square+=static_cast<double>(c.square);
        }
    }
    const double mean=sum/total;
    const double scale=std::sqrt(std::max(1.,square/total-mean*mean));
    std::array<int,8> order{0,1,2,3,4,5,6,7};
    if (removeOrder) {
        std::array<double,8> brightness{};
        for (int x=0;x<8;++x) for (int s=0;s<4;++s) {
            const auto& c=cells[s*8+x];
            brightness[x]+=static_cast<double>(c.sum)/c.n;
        }
        std::stable_sort(order.begin(),order.end(),[&](int a,int b){return brightness[a]<brightness[b];});
    }
    for (int strip=0;strip<4;++strip) {
        std::array<double,channels> averages{};
        for (int x=0;x<8;++x) {
            const auto& c=cells[strip*8+order[x]];
            const double local=static_cast<double>(c.sum)/c.n;
            std::array<double,channels> values{(local-mean)/scale,std::sqrt(std::max(0.,static_cast<double>(c.square)/c.n-local*local))/scale};
            for (int j=0;j<6;++j) values[2+j]=c.edge[j]/(c.n*scale);
            for (int j=0;j<channels;++j) {
                result.push_back(static_cast<float>(values[j]));
                averages[j]+=values[j]/8;
            }
        }
        for (auto value:averages) result.push_back(static_cast<float>(value));
    }
}

// 수치형 회귀 트리의 값을 비교 측정하기 위한 읽기 전용 예측 경로.
class PackedForest {
    struct Node { int feature=-1, left=0; double value=0; };
    std::vector<Node> nodes_;
    std::vector<int> roots_;
    template<class Forest>
    void copy_node(const Forest& forest,int src,int dst) {
        const auto& original=forest.getNodes().at(src);
        if (original.split<0) { nodes_[dst].value=original.value; return; }
        const auto& split=forest.getSplits().at(original.split);
        if (split.inversed || split.varIdx<0 || split.varIdx>=forest.getVarCount())
            throw std::runtime_error("Unsupported split");
        const int left=static_cast<int>(nodes_.size());
        nodes_[dst]={split.varIdx,left,split.c};
        nodes_.resize(nodes_.size()+2);
        copy_node(forest,original.left,left);
        copy_node(forest,original.right,left+1);
    }
public:
    // 회귀 모델의 앞 limit개 트리를 복사한다. 지정한 수가 유효하지 않거나 분류 모델이면 거부한다.
    // 기존 트리를 교체하지 않으므로 새 비교를 시작할 때는 새 객체를 사용한다.
    template<class Forest>
    void load(const Forest& forest, int limit) {
        if (forest.isClassifier() || forest.getRoots().empty()) throw std::runtime_error("Expected regression trees");
        if (limit<1 || limit>static_cast<int>(forest.getRoots().size())) throw std::invalid_argument("Invalid tree count");
        nodes_.reserve(forest.getNodes().size());
        for (int t=0;t<limit;++t) {
            const int r=forest.getRoots()[t];
            roots_.push_back(static_cast<int>(nodes_.size()));
            nodes_.emplace_back();
            copy_node(forest,r,roots_.back());
        }
    }
    // load 이후 모델 학습과 같은 순서의 float 특징 행을 받는다. 각 트리 결과의 평균을 반환한다.
    float predict(const cv::Mat& x) const {
        const float* values=x.ptr<float>();
        double sum=0;
        for (int i:roots_) {
            while (nodes_[i].feature>=0) {
                const auto& n=nodes_[i];
                i=n.left+(values[n.feature]>n.value);
            }
            sum+=nodes_[i].value;
        }
        return static_cast<float>(sum)*(1.f/static_cast<float>(roots_.size()));
    }
};
}
