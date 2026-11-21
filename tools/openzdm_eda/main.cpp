#include "StudyMeasurements.h"
#include "PatternStudy.h"
#include "InspectionInput.h"

using namespace study;

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc != 5) throw std::invalid_argument("Usage: openzdm_eda_final observe|global|center manifest.json config.json output.csv");
        const std::wstring feature = argv[1];
        const bool isObservation = feature == L"observe";
        const bool training = feature == L"pattern-train" || feature == L"pattern-train-stable";
        const bool patternScores = feature == L"pattern-scores";
        const int modelIndex = feature == L"pattern-A" ? 0 : feature == L"pattern-B" ? 1 : feature == L"pattern-C" ? 2 : -1;
        if (!isObservation && !training && !patternScores && modelIndex < 0 && feature != L"global" && feature != L"center")
            throw std::invalid_argument("Unknown feature");
        cv::setNumThreads(1);
        const fs::path manifestPath = argv[2];
        const auto manifest = read_json(read_bytes(manifestPath, 32 * 1024 * 1024));
        const auto root = manifestPath.parent_path() / fs::u8path(manifest["root"].string());
        const auto config = read_json(read_bytes(argv[3], config_file_limit));
        const auto settings = parse_inspection_parameters(config, InspectionMode::Selective);
        const SelectiveInspectionPolicy policy(settings.fastBand, settings.preciseBand, settings.budget);
        auto fallback = settings.fastBand;
        if (modelIndex >= 0) {
            fallback = {config["outside_pass"].real(), config["outside_fail"].real()};
        }
        const SelectiveInspectionPolicy outside(fallback, settings.preciseBand, settings.budget);
        std::array<pattern_study::Model,3> models;
        if (modelIndex >= 0 || patternScores) models = pattern_study::load(config);
        const auto rows = manifest["rows"];
        if (!rows.isSeq() || rows.empty()) throw std::invalid_argument("Missing manifest rows");
        if (fs::exists(argv[4])) throw std::invalid_argument("Use a new output CSV");
        std::vector<pattern_study::Features> samples;
        std::vector<int> labels;
        std::ofstream output;
        if (!training) output.open(fs::path(argv[4]), std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output << std::setprecision(std::numeric_limits<double>::max_digits10);
        if (!training) {
            output << "path,label,width,height,";
            if (isObservation) output << "global_score,global_ns,center_score,center_ns,precise_score,precise_ns\n";
            else if (patternScores) output << "A,B,C\n";
            else output << "fast_score,fast_ns,precise_score,precise_ns,decision,reason\n";
        }
        for (const auto& row : rows) {
            const auto path = row["path"].string();
            const auto label = row["label"].string();
            const auto bytes = read_bytes(root / fs::u8path(path), image_file_limit);
            if (sha256(bytes) != row["sha256"].string()) throw std::runtime_error("Input hash differs: " + path);
            cv::Mat image;
            try {
                image = decode_png(bytes);
            } catch (const InspectionInputError&) {}
            if (training) {
                if (row["split"].string() != "train" || (label!="KO" && label!="OK")) throw std::runtime_error("Invalid training row");
                samples.push_back(pattern_study::extract(image, pattern_study::lengths[2]));
                labels.push_back(label=="KO");
                continue;
            }
            output << path << ',' << label << ',' << image.cols << ',' << image.rows << ',';
            if (isObservation) {
                write_measurement(output, observe(image, settings.threshold, LocalContrastMode::Fast));
                output << ',';
                write_measurement(output, observe(image, settings.threshold, LocalContrastMode::Fast, true));
                output << ',';
                write_measurement(output, observe(image, settings.threshold, LocalContrastMode::Precise));
            } else if (patternScores) {
                const auto sample = pattern_study::extract(image,pattern_study::lengths[2]);
                output << models[0].score(sample) << ',' << models[1].score(sample) << ',' << models[2].score(sample);
            } else {
                const bool target = modelIndex >= 0 && image.cols >= 385 && image.cols <= 415;
                const auto fast = [&] {
                    if (!target) return observe(image, settings.threshold, LocalContrastMode::Fast, feature == L"center");
                    const auto start = Clock::now();
                    const double score = models[modelIndex].score(pattern_study::extract(image,pattern_study::lengths[modelIndex]));
                    return AnalysisMeasurement::from_nanoseconds(score, duration_cast<nanoseconds>(Clock::now()-start));
                }();
                const auto& activePolicy = modelIndex >= 0 && !target ? outside : policy;
                const auto result = activePolicy.evaluate(fast, [&] { return observe(image, settings.threshold, LocalContrastMode::Precise); });
                write_measurement(output, fast);
                output << ',';
                if (result.precise) write_measurement(output, *result.precise);
                else output << ',';
                output << ',' << outcome_name(result.outcome) << ',' << reason_name(result.reason);
            }
            output << '\n';
        }
        if (training) pattern_study::train(samples,labels,argv[4],feature==L"pattern-train-stable");
        else output.close();
        std::cout << rows.size() << " images\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
