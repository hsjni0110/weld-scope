#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "BaselineInspection.h"
#include "InspectionFiles.h"
#include "InspectionInput.h"
#include "LocalContrastInspection.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;
using namespace weldscope;
using Clock = std::chrono::steady_clock;
namespace {
struct InputRecord {
    fs::path file;
    std::string path, split, label, expectedHash;
    bool debug = false;
};
struct AnalysisResult {
    cv::Mat gray, mask;
    std::uint64_t candidatePixels, totalPixels;
    double score;
    bool isFail;
    std::optional<double> continuousCandidateRatio;
};
double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
std::string format_score(double value)
{
    std::ostringstream number;
    number << std::setprecision(17) << value;
    return number.str();
}
std::string string_value(const cv::FileNode& node, const char* name)
{
    if (!node.isString()) throw std::invalid_argument(std::string("Missing or nonstring field: ") + name);
    return static_cast<std::string>(node);
}
std::wstring case_key(const std::string& path)
{
    const auto original = fs::u8path(path).wstring();
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, original.c_str(), -1, nullptr, 0, nullptr, nullptr, 0);
    if (!size) throw std::runtime_error("Path normalization failed");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, original.c_str(), -1, result.data(), size, nullptr, nullptr, 0))
        throw std::runtime_error("Path normalization failed");
    return result;
}
std::vector<InputRecord> load_manifest(const fs::path& file)
{
    auto json = read_json(read_bytes(file, 32 * 1024 * 1024));
    if (read_integer(json["schema_version"], "schema_version") != 1) throw std::invalid_argument("Unsupported manifest schema");
    const auto rootName = string_value(json["root"], "root");
    const auto root = contained_path(fs::absolute(file).parent_path(), rootName);
    const auto rows = json["rows"];
    if (!rows.isSeq() || rows.size() == 0 || rows.size() > 100000) throw std::invalid_argument("Invalid manifest rows");
    std::vector<InputRecord> records;
    std::set<std::wstring> seen;
    std::string previous;
    for (const auto& row : rows) {
        InputRecord record;
        record.path = string_value(row["path"], "path");
        record.split = string_value(row["split"], "split");
        record.label = string_value(row["label"], "label");
        record.expectedHash = string_value(row["sha256"], "sha256");
        const int debug = read_integer(row["debug"], "debug");
        record.debug = debug == 1;
        const auto relative = fs::u8path(record.path);
        const auto prefix = "train/" + record.label + "/";
        if (record.split != "train" || (record.label != "OK" && record.label != "KO")
            || record.path.compare(0, prefix.size(), prefix) != 0
            || record.path.find('/', prefix.size()) != std::string::npos
            || relative.filename().u8string().empty()
            || std::distance(relative.begin(), relative.end()) != 3
            || debug < 0 || debug > 1 || record.expectedHash.size() != 64
            || !std::all_of(record.expectedHash.begin(), record.expectedHash.end(), [](char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); })
            || (!previous.empty() && previous >= record.path) || !seen.insert(case_key(record.path)).second)
            throw std::invalid_argument("Invalid, unsorted, duplicate or nontrain manifest row");
        record.file = contained_path(root, record.path);
        previous = record.path;
        records.push_back(std::move(record));
    }
    return records;
}
void export_images(const fs::path& directory, const cv::Mat& image, const cv::Mat& gray, const cv::Mat& mask)
{
    fs::create_directories(directory);
    write_png(directory / "original.png", image);
    write_png(directory / "gray.png", gray);
    write_png(directory / "mask.png", mask);
    write_png(directory / "overlay.png", make_candidate_overlay(image, mask));
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        SetConsoleOutputCP(CP_UTF8);
        cv::setNumThreads(1);
        std::map<std::wstring, fs::path> options;
        const std::set<std::wstring> allowed{L"--config", L"--output", L"--image", L"--manifest", L"--algorithm"};
        if (argc == 2 && std::wstring(argv[1]) == L"--help") {
            std::cout << "weldscope_inspect [--algorithm baseline|fast|precise] --config config.json "
                         "(--image file.png | --manifest manifest.json) --output NEW_DIRECTORY\n";
            return 0;
        }
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc || !allowed.count(argv[index]) || !options.emplace(argv[index], argv[index + 1]).second)
                throw std::invalid_argument("Unknown, duplicate or incomplete argument; use --help");
        }
        if (!options.count(L"--config") || !options.count(L"--output")
            || options.count(L"--image") + options.count(L"--manifest") != 1)
            throw std::invalid_argument("Require config, output and exactly one input mode");
        std::optional<LocalContrastMode> localMode;
        const char* algorithmVersion = "global-dark-ratio-v1";
        if (const auto algorithm = options.find(L"--algorithm"); algorithm != options.end()) {
            if (algorithm->second == L"fast") {
                localMode = LocalContrastMode::Fast;
                algorithmVersion = "local-contrast-fast-v1";
            } else if (algorithm->second == L"precise") {
                localMode = LocalContrastMode::Precise;
                algorithmVersion = "local-contrast-precise-v1";
            } else if (algorithm->second != L"baseline") {
                throw std::invalid_argument("Unknown algorithm; use --help");
            }
        }
        const auto configBytes = read_bytes(options.at(L"--config"), config_file_limit);
        auto json = read_json(configBytes);
        const char* thresholdKey = localMode ? "contrast_threshold" : "intensity_threshold";
        std::optional<BaselineConfig> baselineConfig;
        int threshold;
        double failRatio;
        if (localMode) {
            const auto settings = read_inspection_settings(json, thresholdKey);
            threshold = settings.threshold;
            failRatio = settings.failRatio;
            validate_local_contrast_config(threshold, failRatio);
        } else {
            baselineConfig = parse_baseline_config(json);
            threshold = baselineConfig->intensityThreshold();
            failRatio = baselineConfig->failRatio();
        }
        const auto analyze = [&](const cv::Mat& image, double& analysisMs) -> AnalysisResult {
            const auto start = Clock::now();
            if (localMode) {
                auto result = inspect_local_contrast(image, *localMode, threshold, failRatio);
                analysisMs = elapsed(start);
                return {std::move(result.gray), std::move(result.mask), result.candidatePixels, result.totalPixels,
                        result.score, result.isFail, result.continuousCandidateRatio};
            }
            auto result = inspect_baseline(image, *baselineConfig);
            analysisMs = elapsed(start);
            return {std::move(result.gray), std::move(result.mask), result.candidatePixels, result.totalPixels,
                    result.score, result.decision == BaselineDecision::Fail, std::nullopt};
        };
        const auto configHash = sha256(configBytes);
        std::vector<InputRecord> records;
        if (options.count(L"--manifest")) records = load_manifest(options.at(L"--manifest"));
        else {
            const auto image = fs::absolute(options.at(L"--image"));
            records.push_back({image, image.u8string(), "", "", "", true});
        }
        const auto output = fs::absolute(options.at(L"--output"));
        if (fs::exists(output) || !fs::create_directories(output)) throw std::runtime_error("Output must be a new directory");
        write_text(output / "config.json", std::string(configBytes.begin(), configBytes.end()));
        std::ofstream csv(output / "results.csv", std::ios::binary);
        csv.exceptions(std::ios::failbit | std::ios::badbit);
        csv << "source_path,split,label,image_sha256,config_sha256,width,height,candidate_pixels,total_pixels,"
               "score,decision,error,load_ms,analysis_ms,artifact_ms,artifact_path";
        if (localMode) csv << ",continuous_candidate_ratio";
        csv << '\n';
        int passed = 0, failed = 0, errors = 0;
        std::size_t ordinal = 0;
        for (const auto& record : records) {
            ++ordinal;
            std::string imageHash, decision = "ERROR", reason, artifactPath;
            std::string width, height, candidates, total, score, continuousCandidateRatio;
            double loadMs = 0, analysisMs = 0, artifactMs = 0;
            cv::Mat image;
            AnalysisResult result{};
            const auto loadStart = Clock::now();
            try {
                const auto bytes = read_bytes(record.file, image_file_limit);
                imageHash = sha256(bytes);
                if (!record.expectedHash.empty() && record.expectedHash != imageHash) throw std::runtime_error("Image SHA-256 mismatch");
                image = decode_png(bytes);
                loadMs = elapsed(loadStart);
                result = analyze(image, analysisMs);
                width = std::to_string(image.cols);
                height = std::to_string(image.rows);
                candidates = std::to_string(result.candidatePixels);
                total = std::to_string(result.totalPixels);
                score = format_score(result.score);
                if (result.continuousCandidateRatio)
                    continuousCandidateRatio = format_score(*result.continuousCandidateRatio);
                decision = result.isFail ? "FAIL" : "PASS";
            } catch (const std::exception& error) {
                reason = error.what();
                if (loadMs == 0) loadMs = elapsed(loadStart);
            }
            // Saving is outside the input-error catch: a disk failure aborts the run with exit 1.
            if (decision != "ERROR" && record.debug) {
                const auto start = Clock::now();
                std::ostringstream name;
                name << "images/" << std::setfill('0') << std::setw(6) << ordinal;
                artifactPath = name.str();
                export_images(output / fs::u8path(artifactPath), image, result.gray, result.mask);
                artifactMs = elapsed(start);
            }
            if (decision == "PASS") ++passed;
            else if (decision == "FAIL") ++failed;
            else ++errors;
            const std::string fields[] = {record.path, record.split, record.label, imageHash, configHash, width, height,
                candidates, total, score, decision, reason};
            for (const auto& field : fields) csv << csv_field(field) << ',';
            csv << std::setprecision(17) << loadMs << ',' << analysisMs << ',' << artifactMs << ',' << csv_field(artifactPath);
            if (localMode) csv << ',' << continuousCandidateRatio;
            csv << '\n';
        }
        csv.close();
        cv::FileStorage summary(".json", cv::FileStorage::WRITE | cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
        summary << "algorithm_version" << algorithmVersion << "opencv_version" << CV_VERSION
                << "scope" << (options.count(L"--manifest") ? "train-development-only" : "unlabelled-single-image") << "independent_evaluation" << 0
                << "requested" << static_cast<int>(records.size()) << "PASS" << passed << "FAIL" << failed << "ERROR" << errors
                << "threads" << cv::getNumThreads() << "config_sha256" << configHash
                << thresholdKey << threshold << "fail_ratio" << failRatio;
        write_text(output / "summary.json", summary.releaseAndGetString());
        std::cout << "Requested " << records.size() << ", PASS " << passed << ", FAIL " << failed << ", ERROR " << errors << '\n'
                  << "Results: " << output.u8string() << '\n';
        return errors ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
