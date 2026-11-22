#include "AnalysisPipeline.h"
#include "InspectionInput.h"
#include "PooledSmoothModel.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace weldscope;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_score_limits() {
    const ScoreBand band{0.2, 0.8};
    require(classify_score(0.2, band) == InspectionOutcome::Pass, "normal equality");
    require(classify_score(0.8, band) == InspectionOutcome::Fail, "defect equality");
    require(classify_score(0.5, band) == InspectionOutcome::Review, "review between thresholds");
    require(classify_score(std::numeric_limits<double>::quiet_NaN(), band) == InspectionOutcome::Error, "invalid score");
}

void test_frozen_model(const std::filesystem::path& directory, const std::filesystem::path& sample) {
    auto manifest = read_bytes(directory / pooledSmoothConfigFile, config_file_limit);
    const PooledSmoothModel model(directory, manifest);
    const auto image = decode_png(read_bytes(sample, image_file_limit));
    const auto output = analyze_image(image, model);
    require(output.outcome == InspectionOutcome::Fail && output.score &&
        std::abs(*output.score - 0.7688432614002394) < 1e-12, "frozen defect score and verdict");
    const auto sameImage = analyze_image(image, model);
    require(sameImage.outcome == output.outcome && sameImage.score == output.score,
        "LightGBM classification must depend on the score, never the clock");
    require(analyze_image(cv::Mat{}, model).outcome == InspectionOutcome::Error, "invalid image");
    require(model.band(415).passMax > model.band(416).passMax &&
        model.band(416).passMax > model.band(422).passMax, "smooth calibration across the width boundary");

    const auto emptyDir = std::filesystem::temp_directory_path() /
        ("weldscope-model-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(emptyDir), "could not create a model fixture directory");
    try {
        const PooledSmoothModel missing(emptyDir, manifest);
        require(false, "missing model accepted");
    } catch (const InspectionInputError& error) {
        require(error.reason() == FileInspectionFailure::FileAccess, "unexpected missing model error");
    }
    const auto weights = emptyDir / "model-220.txt";
    std::filesystem::copy_file(directory / weights.filename(), weights);
    std::ofstream(weights, std::ios::app).put('\n');
    try {
        const PooledSmoothModel corrupt(emptyDir, manifest);
        require(false, "altered model accepted");
    } catch (const std::invalid_argument&) {}
    std::filesystem::remove(weights);
    std::filesystem::remove(emptyDir);
    manifest.push_back(' ');
    try {
        const PooledSmoothModel altered(directory, manifest);
        require(false, "altered manifest accepted for frozen model");
    } catch (const std::invalid_argument&) {}
}
}

int main(int argc, char** argv) {
    try {
        test_score_limits();
        require(argc == 3, "expected the frozen model directory and defect sample");
        test_frozen_model(argv[1], argv[2]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
