#include "PatternStudy.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace pattern_study {
namespace {
double logistic(double x) { return 1.0 / (1.0 + std::exp(-std::clamp(x, -40.0, 40.0))); }

// Same mean binary cross-entropy and L2 penalty used by the original 80 updates.
double objective(const cv::Mat& design, const std::vector<int>& labels, const cv::Mat& weights, cv::Mat& gradient)
{
    cv::Mat residual = design*weights;
    double loss = 0;
    for (int i=0; i<design.rows; ++i) {
        const double z = residual.at<double>(i);
        loss += std::max(z,0.0) - z*labels[i] + std::log1p(std::exp(-std::abs(z)));
        residual.at<double>(i) = logistic(z)-labels[i];
    }
    loss /= design.rows;
    gradient = (design.t()*residual)/design.rows;
    for (int j=1; j<weights.rows; ++j) {
        const double w = weights.at<double>(j);
        loss += .01*w*w;
        gradient.at<double>(j) += .02*w;
    }
    return loss;
}

void converge(const cv::Mat& design, const std::vector<int>& labels, cv::Mat& weights, cv::FileStorage& file)
{
    cv::Mat gradient;
    double loss = objective(design,labels,weights,gradient);
    file << "initial_loss" << loss << "initial_max_gradient" << cv::norm(gradient,cv::NORM_INF);
    cv::Mat inverse = cv::Mat::eye(weights.rows,weights.rows,CV_64F);
    int steps = 0;
    while (cv::norm(gradient,cv::NORM_INF)>1e-6 && steps<200) {
        const cv::Mat descent = -inverse*gradient;
        const double slope = descent.dot(gradient);
        if (slope>=0) throw std::runtime_error("Training direction is not descending");
        cv::Mat candidate, nextGradient;
        double nextLoss, step = 1.0;
        for (;;) {
            candidate = weights + step*descent;
            nextLoss = objective(design,labels,candidate,nextGradient);
            if (nextLoss<=loss+.0001*step*slope) break;
            step *= .5;
            if (step<1e-12) throw std::runtime_error("Training line search failed");
        }
        const cv::Mat s = candidate-weights, y = nextGradient-gradient;
        const double curvature = y.dot(s);
        if (curvature>1e-14) {
            const cv::Mat hy = inverse*y;
            inverse += ((curvature+y.dot(hy))/(curvature*curvature))*(s*s.t())
                - (hy*s.t()+s*hy.t())/curvature;
        }
        weights = candidate;
        gradient = nextGradient;
        loss = nextLoss;
        ++steps;
    }
    file << "final_loss" << loss << "final_max_gradient" << cv::norm(gradient,cv::NORM_INF) << "steps" << steps;
    if (cv::norm(gradient,cv::NORM_INF)>1e-6) throw std::runtime_error("Training convergence criterion unmet");
}

void scale_features(const cv::Mat& gray, int radius, bool spatial, double* out)
{
    const int quarter = gray.rows / 4;
    const int vertical = std::min(radius, (quarter - 1) / 2);
    if (vertical < 1 || gray.cols <= 2 * radius + 8) throw std::invalid_argument("Image too small");
    for (int half = 0; half < 2; ++half) {
        const int top = half ? gray.rows - 2 * quarter : quarter;
        int counts[8][16]{};
        double area[8]{};
        for (int tile = 0; tile < 8; ++tile) {
            const int left = radius + (gray.cols - 2 * radius) * tile / 8;
            const int right = radius + (gray.cols - 2 * radius) * (tile + 1) / 8;
            area[tile] = (right - left) * (quarter - 2 * vertical);
            for (int y = top + vertical; y < top + quarter - vertical; ++y) {
                const auto* mid = gray.ptr<unsigned char>(y);
                const auto* up = gray.ptr<unsigned char>(y - vertical);
                const auto* down = gray.ptr<unsigned char>(y + vertical);
                for (int x = left; x < right; ++x) {
                    const int v = mid[x];
                    const int code = (mid[x-radius]>v) | ((mid[x+radius]>v)<<1)
                        | ((up[x]>v)<<2) | ((down[x]>v)<<3);
                    ++counts[tile][code];
                }
            }
        }
        for (int k = 0; k < 16; ++k) {
            const int column = half * 16 + k;
            int total = 0;
            double peak = 0, previous = 0, variation = 0;
            for (int t = 0; t < 8; ++t) {
                total += counts[t][k];
                if (spatial) {
                    const double density = counts[t][k] / area[t];
                    peak = std::max(peak, density);
                    if (t) variation += std::abs(density - previous);
                    previous = density;
                }
            }
            out[column] = static_cast<double>(total) / ((gray.cols-2*radius)*(quarter-2*vertical));
            if (spatial) {
                out[32+column] = peak-out[column];
                out[64+column] = variation/7;
            }
        }
    }
}
}

Features extract(const cv::Mat& image, int length)
{
    cv::Mat gray;
    if (image.type() == CV_8UC1) gray = image;
    else cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    Features result{};
    scale_features(gray, 1, length >= lengths[1], result.data());
    if (length == lengths[2]) scale_features(gray, 3, true, result.data()+96);
    return result;
}

double Model::score(const Features& values) const
{
    double output = intercept;
    for (size_t j = 0; j < weights.size(); ++j)
        output += weights[j]*std::clamp((values[j]-mean[j])/scale[j], -5.0, 5.0);
    return logistic(output);
}

std::array<Model,3> load(const cv::FileStorage& file)
{
    std::array<Model,3> models;
    for (int i = 0; i < 3; ++i) {
        auto& m = models[i];
        const auto node = file["models"][i];
        node["mean"] >> m.mean;
        node["scale"] >> m.scale;
        node["weights"] >> m.weights;
        m.intercept = node["intercept"].real();
        if (m.weights.size()!=static_cast<size_t>(lengths[i]) || m.mean.size()!=m.weights.size()
            || m.scale.size()!=m.weights.size() || *std::min_element(m.scale.begin(),m.scale.end())<=0)
            throw std::invalid_argument("Invalid pattern model");
    }
    return models;
}

void train(const std::vector<Features>& rows, const std::vector<int>& labels,
           const std::filesystem::path& destination, bool checkConvergence)
{
    const int n = static_cast<int>(rows.size());
    cv::FileStorage file("",cv::FileStorage::WRITE|cv::FileStorage::MEMORY|cv::FileStorage::FORMAT_JSON);
    file << "models" << "[";
    for (const int width : lengths) {
        cv::Mat design(n,width+1,CV_64F,cv::Scalar(1));
        std::vector<double> mean(width), scale(width);
        for (const auto& row : rows)
            for (int j=0; j<width; ++j) mean[j] += row[j]/n;
        for (const auto& row : rows)
            for (int j=0; j<width; ++j) scale[j] += std::pow(row[j]-mean[j],2)/n;
        for (auto& s : scale) s = s>0 ? std::sqrt(s) : 1.0;
        for (int i=0; i<n; ++i)
            for (int j=0; j<width; ++j)
                design.at<double>(i,j+1)=std::clamp((rows[i][j]-mean[j])/scale[j],-5.0,5.0);
        cv::Mat weights=cv::Mat::zeros(width+1,1,CV_64F);
        for (int iter=0; iter<80; ++iter) {
            cv::Mat errors=design*weights;
            for (int i=0; i<n; ++i) errors.at<double>(i)=logistic(errors.at<double>(i))-labels[i];
            cv::Mat gradient=(design.t()*errors)/n;
            for (int j=0; j<=width; ++j)
                weights.at<double>(j)-=.35*(gradient.at<double>(j)+(j ? .02*weights.at<double>(j) : 0));
        }
        file << "{";
        if (checkConvergence) converge(design,labels,weights,file);
        std::vector<double> coefficients(weights.ptr<double>()+1, weights.ptr<double>()+width+1);
        file << "intercept" << weights.at<double>(0) << "mean" << mean << "scale" << scale
             << "weights" << coefficients << "}";
    }
    file << "]";
    std::ofstream output(destination);
    output.exceptions(std::ios::badbit|std::ios::failbit);
    output << file.releaseAndGetString();
}
}
