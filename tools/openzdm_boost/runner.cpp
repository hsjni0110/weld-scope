#include "BoostedTrees.h"
#include "InspectionInput.h"
#include "../openzdm_eda/StudyMeasurements.h"

int wmain(int argc, wchar_t** argv)
{
    using namespace study;
    try {
        if (argc != 5) throw std::invalid_argument("Use extract|measure manifest config output");
        cv::setNumThreads(1);
        const std::wstring mode = argv[1];
        const fs::path manifest = argv[2], configPath = argv[3], destination = argv[4];
        if (mode != L"extract" && mode != L"measure") throw std::invalid_argument("Unknown mode");
        if (fs::exists(destination)) throw std::invalid_argument("Choose a new output");
        const auto input = read_json(read_bytes(manifest,32*1024*1024));
        const auto root = manifest.parent_path()/fs::u8path(input["root"].string());
        const auto config = read_json(read_bytes(configPath,config_file_limit));
        const auto settings = parse_inspection_parameters(config,InspectionMode::Selective);
        const SelectiveInspectionPolicy policy(settings.fastBand,settings.preciseBand,settings.budget);
        std::optional<pooled_texture::BoostedTrees> model;
        std::ofstream output(destination,std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        if (mode == L"measure") {
            model.emplace(configPath.parent_path()/fs::u8path(config["model"].string()));
            output << std::setprecision(17) << "path,label,width,height,fast_score,fast_ns,precise_score,precise_ns,decision,reason\n";
        } else {
            const int header[]{static_cast<int>(input["rows"].size()),pooled_texture::featureCount};
            output.write(reinterpret_cast<const char*>(header),sizeof(header));
        }
        for (const auto& row : input["rows"]) {
            const auto path = row["path"].string(), label = row["label"].string();
            const auto bytes = read_bytes(root/fs::u8path(path),image_file_limit);
            if (sha256(bytes) != row["sha256"].string()) throw std::invalid_argument("Input hash mismatch: "+path);
            const auto image = decode_png(bytes);
            if (mode == L"extract") {
                if (row["split"].string() != "train" || (label != "KO" && label != "OK"))
                    throw std::invalid_argument("Only train rows allowed in fitting");
                const auto features = pooled_texture::describe(image);
                const float response = label == "KO" ? 1.f : 0.f;
                output.write(reinterpret_cast<const char*>(&response),sizeof(response));
                output.write(reinterpret_cast<const char*>(features.data()),sizeof(features));
                continue;
            }
            const auto start = Clock::now();
            const double score = model->predict(pooled_texture::describe(image));
            const auto fast = AnalysisMeasurement::from_nanoseconds(score,duration_cast<nanoseconds>(Clock::now()-start));
            const auto result = policy.evaluate(fast,[&]{return observe(image,settings.threshold,LocalContrastMode::Precise);});
            output << path << ',' << label << ',' << image.cols << ',' << image.rows << ',';
            write_measurement(output,fast);
            output << ',';
            if (result.precise) write_measurement(output,*result.precise);
            else output << ',';
            output << ',' << outcome_name(result.outcome) << ',' << reason_name(result.reason) << '\n';
        }
        output.close();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
