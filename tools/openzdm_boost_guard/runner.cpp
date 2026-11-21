#include "CompactBoostedTrees.h"
#include "../openzdm_eda/StudyMeasurements.h"

int wmain(int argc,wchar_t** argv)
{
    using namespace study;
    try {
        if (argc != 5 || std::wstring(argv[1]) != L"measure")
            throw std::invalid_argument("Use measure manifest config output");
        cv::setNumThreads(1);
        const fs::path manifest=argv[2], settingsPath=argv[3], destination=argv[4];
        if (fs::exists(destination)) throw std::invalid_argument("Choose a new output");
        const auto input=read_json(read_bytes(manifest,32*1024*1024));
        const auto root=manifest.parent_path()/fs::u8path(input["root"].string());
        const auto config=read_json(read_bytes(settingsPath,config_file_limit));
        const auto settings=parse_inspection_parameters(config,InspectionMode::Selective);
        const SelectiveInspectionPolicy inside(settings.fastBand,settings.preciseBand,settings.budget);
        if (config["outside_pass"].empty() || config["outside_fail"].empty())
            throw std::invalid_argument("Missing width calibration");
        const ScoreBand outsideBand{config["outside_pass"].real(),config["outside_fail"].real()};
        const SelectiveInspectionPolicy outside(outsideBand,settings.preciseBand,settings.budget);
        std::ifstream modelFile(settingsPath.parent_path()/fs::u8path(config["model"].string()));
        const pooled_texture::CompactBoostedTrees model(modelFile);
        std::ofstream output(destination);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << std::setprecision(17) << "path,label,width,height,fast_score,fast_ns,precise_score,precise_ns,decision,reason\n";
        for (const auto& row : input["rows"]) {
            const auto path=row["path"].string(), label=row["label"].string();
            const auto bytes=read_bytes(root/fs::u8path(path),image_file_limit);
            if (sha256(bytes)!=row["sha256"].string()) throw std::invalid_argument("Input hash mismatch: "+path);
            const auto image=decode_png(bytes);
            const auto start=Clock::now();
            const auto& policy=image.cols>=385 && image.cols<=415 ? inside : outside;
            const auto score=model.predict(pooled_texture::describe(image));
            const auto fast=AnalysisMeasurement::from_nanoseconds(score,duration_cast<nanoseconds>(Clock::now()-start));
            const auto result=policy.evaluate(fast,[&]{return observe(image,settings.threshold,LocalContrastMode::Precise);});
            output << path << ',' << label << ',' << image.cols << ',' << image.rows << ',';
            write_measurement(output,fast);
            output << ',';
            if (result.precise) write_measurement(output,*result.precise);
            else output << ',';
            output << ',' << outcome_name(result.outcome) << ',' << reason_name(result.reason) << '\n';
        }
        output.close();
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
    return 0;
}
