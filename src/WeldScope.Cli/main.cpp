#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "BaselineInspection.h"
#include "InspectionFiles.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace weldscope;
using Clock = std::chrono::steady_clock;
namespace {
struct InputRecord {
    fs::path file;
    std::string path, split, label, expectedHash;
    bool debug = false;
};
double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
int integer(const cv::FileNode& node, const char* name)
{
    if (!node.isInt()) throw std::invalid_argument(std::string("Missing or noninteger field: ") + name);
    const auto value = static_cast<std::int64_t>(node);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::invalid_argument(std::string("Integer field out of range: ") + name);
    return static_cast<int>(value);
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
    if (integer(json["schema_version"], "schema_version") != 1) throw std::invalid_argument("Unsupported manifest schema");
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
        const int debug = integer(row["debug"], "debug");
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
void export_images(const fs::path& directory, const cv::Mat& image, const BaselineResult& result)
{
    fs::create_directories(directory);
    write_png(directory / "original.png", image);
    write_png(directory / "gray.png", result.gray);
    write_png(directory / "mask.png", result.mask);
    cv::Mat colored = image.clone(), overlay;
    colored.setTo(cv::Scalar(0, 191, 255), result.mask);
    cv::addWeighted(image, 0.55, colored, 0.45, 0, overlay);
    write_png(directory / "overlay.png", overlay);
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        SetConsoleOutputCP(CP_UTF8);
        cv::setNumThreads(1);
        std::map<std::wstring, fs::path> options;
        const std::set<std::wstring> allowed{L"--config", L"--output", L"--image", L"--manifest"};
        if (argc == 2 && std::wstring(argv[1]) == L"--help") {
            std::cout << "weldscope_inspect --config config.json (--image file.png | --manifest manifest.json) --output NEW_DIRECTORY\n";
            return 0;
        }
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc || !allowed.count(argv[index]) || !options.emplace(argv[index], argv[index + 1]).second)
                throw std::invalid_argument("Unknown, duplicate or incomplete argument; use --help");
        }
        if (!options.count(L"--config") || !options.count(L"--output")
            || options.count(L"--image") + options.count(L"--manifest") != 1)
            throw std::invalid_argument("Require config, output and exactly one input mode");
        const auto configBytes = read_bytes(options.at(L"--config"), 1024 * 1024);
        auto json = read_json(configBytes);
        if (integer(json["schema_version"], "schema_version") != 1) throw std::invalid_argument("Unsupported config schema");
        if (!json["fail_ratio"].isInt() && !json["fail_ratio"].isReal()) throw std::invalid_argument("Numeric fail_ratio required");
        const BaselineConfig config(integer(json["intensity_threshold"], "intensity_threshold"), static_cast<double>(json["fail_ratio"]));
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
        csv << "source_path,split,label,image_sha256,config_sha256,width,height,candidate_pixels,total_pixels,score,decision,error,load_ms,analysis_ms,artifact_ms,artifact_path\n";
        int passed = 0, failed = 0, errors = 0;
        std::size_t ordinal = 0;
        for (const auto& record : records) {
            ++ordinal;
            std::string imageHash, decision = "ERROR", reason, artifactPath;
            std::string width, height, candidates, total, score;
            double loadMs = 0, analysisMs = 0, artifactMs = 0;
            cv::Mat image;
            BaselineResult result;
            auto start = Clock::now();
            try {
                const auto bytes = read_bytes(record.file, 16 * 1024 * 1024);
                imageHash = sha256(bytes);
                if (!record.expectedHash.empty() && record.expectedHash != imageHash) throw std::runtime_error("Image SHA-256 mismatch");
                image = decode_png(bytes);
                loadMs = elapsed(start);
                start = Clock::now();
                result = inspect_baseline(image, config);
                analysisMs = elapsed(start);
                width = std::to_string(image.cols);
                height = std::to_string(image.rows);
                candidates = std::to_string(result.candidatePixels);
                total = std::to_string(result.totalPixels);
                std::ostringstream number;
                number << std::setprecision(17) << result.score;
                score = number.str();
                decision = result.decision == BaselineDecision::Fail ? "FAIL" : "PASS";
            } catch (const std::exception& error) {
                reason = error.what();
                if (loadMs == 0) loadMs = elapsed(start);
            }
            // Saving is outside the input-error catch: a disk failure aborts the run with exit 1.
            if (decision != "ERROR" && record.debug) {
                start = Clock::now();
                std::ostringstream name;
                name << "images/" << std::setfill('0') << std::setw(6) << ordinal;
                artifactPath = name.str();
                export_images(output / fs::u8path(artifactPath), image, result);
                artifactMs = elapsed(start);
            }
            if (decision == "PASS") ++passed;
            else if (decision == "FAIL") ++failed;
            else ++errors;
            const std::string fields[] = {record.path, record.split, record.label, imageHash, configHash, width, height,
                candidates, total, score, decision, reason};
            for (const auto& field : fields) csv << csv_field(field) << ',';
            csv << std::setprecision(17) << loadMs << ',' << analysisMs << ',' << artifactMs << ',' << csv_field(artifactPath) << '\n';
        }
        csv.close();
        cv::FileStorage summary(".json", cv::FileStorage::WRITE | cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
        summary << "algorithm_version" << "global-dark-ratio-v1" << "opencv_version" << CV_VERSION
                << "scope" << (options.count(L"--manifest") ? "train-development-only" : "unlabelled-single-image") << "independent_evaluation" << 0
                << "requested" << static_cast<int>(records.size()) << "PASS" << passed << "FAIL" << failed << "ERROR" << errors
                << "threads" << cv::getNumThreads() << "config_sha256" << configHash
                << "intensity_threshold" << config.intensityThreshold() << "fail_ratio" << config.failRatio();
        write_text(output / "summary.json", summary.releaseAndGetString());
        std::cout << "Requested " << records.size() << ", PASS " << passed << ", FAIL " << failed << ", ERROR " << errors << '\n'
                  << "Results: " << output.u8string() << '\n';
        return errors ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
