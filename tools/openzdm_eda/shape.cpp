#include "PatternStudy.h"
#include "SpatialForest.h"
#include "StudyMeasurements.h"
#include "InspectionInput.h"
#include <opencv2/ml.hpp>
#include <algorithm>
#include <numeric>

namespace {
using namespace study;
using weldscope::spatial::tile_features;
using weldscope::spatial::PackedForest;
constexpr int columns = 48, height = 24;
using Profile = std::array<float, columns>;
constexpr std::array<const char*,3> names{"texture","shape","combined"};
constexpr std::array<const char*,3> tileNames{"outer","center","whole"};

// Average within small groups while keeping an ordered record of the seam.
void describe(const Profile& profile, std::vector<float>& features)
{
    Profile sorted = profile;
    std::sort(sorted.begin(), sorted.end());
    for (int i : {0,4,12,24,35,43,47}) features.push_back(sorted[i]);
    float change = 0;
    for (int x=1; x<columns; ++x) change += std::abs(profile[x]-profile[x-1]);
    features.push_back(change/(columns-1));
    for (int x=0; x<columns; x+=6)
        features.push_back(std::accumulate(profile.begin()+x,profile.begin()+x+6,0.f)/6);
}

std::vector<float> cross_sections(const cv::Mat& gray)
{
    cv::Mat reduced;
    cv::resize(gray,reduced,{columns,height},0,0,cv::INTER_AREA);
    cv::Scalar mean, deviation;
    cv::meanStdDev(reduced,mean,deviation);
    const float scale=static_cast<float>(std::max(1.,deviation[0]));
    std::array<Profile,8> profiles{};
    for (int x=0; x<columns; ++x) {
        std::array<float,height> p{};
        for (int y=0; y<height; ++y) p[y]=reduced.ptr<unsigned char>(y)[x];
        int center=height/2;
        float contrast=-256;
        for (int y=4; y<height-4; ++y) {
            const float dark=(p[y-1]+2*p[y]+p[y+1])/4;
            const float sides=(p[y-4]+p[y+4])/2;
            if (sides-dark>contrast) { center=y; contrast=sides-dark; }
        }
        const float half=p[center]+std::max(0.f,contrast)/2;
        int top=center, bottom=center;
        while (top>0 && p[top-1]<half) --top;
        while (bottom+1<height && p[bottom+1]<half) ++bottom;
        profiles[0][x]=static_cast<float>(center);
        profiles[1][x]=static_cast<float>(bottom-top+1);
        profiles[2][x]=contrast/scale;
        profiles[3][x]=(p[center-4]-p[center+4])/scale;
        profiles[4][x]=(p[center]-static_cast<float>(mean[0]))/scale;
        profiles[5][x]=static_cast<float>(center-top);
        profiles[6][x]=(p[center+1]+p[center-1]-2*p[center])/scale;
        profiles[7][x]=((p[center-2]+p[center+2])/2-p[center])/scale;
    }
    std::vector<float> features;
    for (const auto& profile : profiles) describe(profile,features);
    return features;
}

cv::Mat features(const cv::Mat& image, int variant, bool tiles, bool removeOrder)
{
    cv::Mat gray;
    if (image.type()==CV_8UC1) gray=image;
    else cv::cvtColor(image,gray,cv::COLOR_BGR2GRAY);
    std::vector<float> result;
    result.reserve(tiles?576:320);
    if (tiles) {
        if (variant!=1) tile_features(gray,true,removeOrder,result);
        if (variant!=0) tile_features(gray,false,removeOrder,result);
        return cv::Mat(result,true).reshape(1,1);
    }
    if (variant!=1) {
        const auto texture=pattern_study::extract(gray,pattern_study::lengths.back());
        for (double v:texture) result.push_back(static_cast<float>(v));
    }
    if (variant!=0) {
        const auto shape=cross_sections(gray);
        result.insert(result.end(),shape.begin(),shape.end());
    }
    return cv::Mat(result,true).reshape(1,1);
}

using Forest = cv::ml::RTrees;
int tree_count(const cv::FileStorage& config, const Forest& model)
{
    return config["tree_count"].empty()?static_cast<int>(model.getRoots().size()):static_cast<int>(config["tree_count"]);
}
void train(const cv::Mat& all, const cv::Mat& labels, const fs::path& folder, bool tiles, bool wholeOnly)
{
    for (int k=wholeOnly?2:0;k<3;++k) {
        const int split=tiles?all.cols/2:192;
        const cv::Mat x=k==0?all.colRange(0,split):k==1?all.colRange(split,all.cols):all;
        cv::setRNGSeed(1729);
        auto model=Forest::create();
        model->setMaxDepth(14);
        model->setMinSampleCount(10);
        model->setActiveVarCount(16);
        model->setCVFolds(0);
        model->setRegressionAccuracy(0);
        model->setTermCriteria({cv::TermCriteria::COUNT,96,0});
        // Continuous 0/1 response gives a leaf average instead of quantized majority votes.
        if (!model->train(x,cv::ml::ROW_SAMPLE,labels)) throw std::runtime_error("Forest fit failed");
        model->save((folder/(std::string((tiles?tileNames:names)[k])+".xml.gz")).string());
    }
}
}

int wmain(int argc,wchar_t** argv)
{
    using namespace study;
    try {
        if (argc!=5) throw std::invalid_argument("Use fit|scores|texture|shape|combined manifest config output");
        cv::setNumThreads(1);
        const std::wstring mode=argv[1];
        const bool fit=mode==L"fit" || mode==L"fit-whole", scores=mode==L"scores", profile=mode==L"profile";
        const int variant=(mode==L"texture" || mode==L"outer")?0:(mode==L"shape" || mode==L"center")?1:2;
        if (!fit && !scores && !profile && mode!=L"combined" && mode!=L"shape" && mode!=L"texture"
            && mode!=L"outer" && mode!=L"center" && mode!=L"whole")
            throw std::invalid_argument("Unknown shape study mode");
        const fs::path input=argv[2], settingsPath=argv[3], destination=argv[4];
        if (fs::exists(destination)) throw std::invalid_argument("Choose a new output");
        const auto list=read_json(read_bytes(input,32*1024*1024));
        const auto root=input.parent_path()/fs::u8path(list["root"].string());
        const auto config=read_json(read_bytes(settingsPath,config_file_limit));
        const bool allWidths=!config["all_widths"].empty() && config["all_widths"].real()!=0;
        const bool removeOrder=config["descriptor"].string()=="sorted";
        const bool tiles=config["descriptor"].string()=="tiles" || removeOrder;
        const auto& modelNames=tiles?tileNames:names;
        const bool pack=!config["packed"].empty() && config["packed"].real()!=0;
        const auto settings=parse_inspection_parameters(config,InspectionMode::Selective);
        const SelectiveInspectionPolicy policy(settings.fastBand,settings.preciseBand,settings.budget);
        const auto outsideBand=config["outside_pass"].empty()?settings.fastBand:
            decltype(settings.fastBand){config["outside_pass"].real(),config["outside_fail"].real()};
        const SelectiveInspectionPolicy outside(outsideBand,settings.preciseBand,settings.budget);
        std::array<cv::Ptr<Forest>,3> models;
        std::array<PackedForest,3> packed;
        PackedForest outsideModel;
        std::optional<SelectiveInspectionPolicy> outsidePolicy;
        if (!config["outside_config"].empty()) {
            if (mode!=L"whole" || !tiles || removeOrder) throw std::invalid_argument("Outside profile requires ordered tiles");
            const auto file=settingsPath.parent_path()/fs::u8path(config["outside_config"].string());
            const auto other=read_json(read_bytes(file,config_file_limit));
            const auto s=parse_inspection_parameters(other,InspectionMode::Selective);
            if (s.threshold!=settings.threshold || s.budget!=settings.budget) throw std::runtime_error("Different runtime policy");
            outsidePolicy.emplace(s.fastBand,s.preciseBand,s.budget);
            const auto model=Forest::load((file.parent_path()/fs::u8path(other["forests"].string())/"whole.xml.gz").string());
            outsideModel.load(*model,tree_count(other,*model));
        }
        std::ofstream csv;
        if (!fit) {
            const auto folder=settingsPath.parent_path()/fs::u8path(config["forests"].string());
            for (int k=0;k<3;++k) if (scores || k==variant) {
                models[k]=Forest::load((folder/(std::string(modelNames[k])+".xml.gz")).string());
                if (pack) packed[k].load(*models[k],tree_count(config,*models[k]));
            }
            csv.exceptions(std::ios::badbit|std::ios::failbit);
            csv.open(destination);
            csv << std::setprecision(17) << "path,label,width,height,";
            if (scores) csv << modelNames[0] << ',' << modelNames[1] << ',' << modelNames[2] << '\n';
            else if (profile) csv << "feature_ns,forest_ns,score\n";
            else csv << "fast_score,fast_ns,precise_score,precise_ns,decision,reason\n";
        }
        cv::Mat training,labels;
        for (const auto& row:list["rows"]) {
            const auto path=row["path"].string(), label=row["label"].string();
            const auto bytes=read_bytes(root/fs::u8path(path),image_file_limit);
            if (sha256(bytes)!=row["sha256"].string()) throw std::runtime_error("Input hash mismatch: "+path);
            const auto image=decode_png(bytes);
            const bool target = allWidths || (image.cols>=385 && image.cols<=415);
            if ((fit || scores) && !target) throw std::runtime_error("Unexpected width");
            if (fit) {
                if (row["split"].string()!="train" || (label!="OK" && label!="KO")) throw std::runtime_error("Invalid training row");
                training.push_back(features(image,2,tiles,removeOrder));
                labels.push_back(label=="KO"?1.f:0.f);
                continue;
            }
            csv << path << ',' << label << ',' << image.cols << ',' << image.rows << ',';
            if (profile) {
                const auto start=Clock::now();
                const auto x=features(image,2,tiles,removeOrder);
                const auto mid=Clock::now();
                const auto score=pack?packed[2].predict(x):models[2]->predict(x);
                const auto end=Clock::now();
                csv << duration_cast<nanoseconds>(mid-start).count() << ',' << duration_cast<nanoseconds>(end-mid).count() << ',' << score << '\n';
            } else if (scores) {
                const auto x=features(image,2,tiles,removeOrder);
                const int split=tiles?x.cols/2:192;
                const auto predict=[&](int index,const cv::Mat& rowFeatures) {
                    return pack?packed[index].predict(rowFeatures):models[index]->predict(rowFeatures);
                };
                csv << predict(0,x.colRange(0,split)) << ',' << predict(1,x.colRange(split,x.cols))
                    << ',' << predict(2,x) << '\n';
            } else {
                const auto fast=[&] {
                    if (!target && !outsidePolicy) return observe(image,settings.threshold,LocalContrastMode::Fast);
                    const auto start=Clock::now();
                    const auto x=features(image,variant,tiles,removeOrder);
                    const auto score=!target?outsideModel.predict(x):pack?packed[variant].predict(x):models[variant]->predict(x);
                    return AnalysisMeasurement::from_nanoseconds(score,duration_cast<nanoseconds>(Clock::now()-start));
                }();
                const auto& activePolicy=target?policy:outsidePolicy?*outsidePolicy:outside;
                const auto result=activePolicy.evaluate(fast,[&]{return observe(image,settings.threshold,LocalContrastMode::Precise);});
                write_measurement(csv,fast);
                csv << ',';
                if (result.precise) write_measurement(csv,*result.precise);
                else csv << ',';
                csv << ',' << outcome_name(result.outcome) << ',' << reason_name(result.reason) << '\n';
            }
        }
        if (fit) { fs::create_directory(destination); train(training,labels,destination,tiles,mode==L"fit-whole"); }
        else csv.close();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
