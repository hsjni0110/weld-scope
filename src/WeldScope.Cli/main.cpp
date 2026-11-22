#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "AnalysisPipeline.h"
#include "InspectionFiles.h"
#include "InspectionInput.h"
#include "PooledSmoothModel.h"
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
};
constexpr std::size_t manifestLimit = 32 * 1024 * 1024;
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
bool is_sha256(const std::string& digest)
{
    return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}
std::vector<InputRecord> load_manifest(const fs::path& file, const std::vector<unsigned char>& bytes,
    const std::string& split)
{
    auto json = read_json(bytes);
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
        const auto relative = fs::u8path(record.path);
        const auto prefix = (split == "validation" ? "val" : split) + "/" + record.label + "/";
        if (record.split != split || (record.label != "OK" && record.label != "KO")
            || record.path.compare(0, prefix.size(), prefix) != 0
            || record.path.find('/', prefix.size()) != std::string::npos
            || relative.filename().u8string().empty()
            || std::distance(relative.begin(), relative.end()) != 3
            || debug < 0 || debug > 1 || !is_sha256(record.expectedHash)
            || (!previous.empty() && previous >= record.path) || !seen.insert(case_key(record.path)).second)
            throw std::invalid_argument("Invalid, unsorted, duplicate or mismatched split manifest row");
        record.file = contained_path(root, record.path);
        previous = record.path;
        records.push_back(std::move(record));
    }
    return records;
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        SetConsoleOutputCP(CP_UTF8);
        cv::setNumThreads(1);
        std::map<std::wstring, fs::path> options;
        const std::set<std::wstring> allowed{L"--config", L"--output", L"--image", L"--manifest", L"--evaluation-split"};
        if (argc == 2 && std::wstring(argv[1]) == L"--help") {
            std::cout << "weldscope_inspect --config config.json (--image file.png | --manifest manifest.json)"
                         " --output NEW_DIRECTORY [--evaluation-split train|validation|test]\n";
            return 0;
        }
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc || !allowed.count(argv[index]) || !options.emplace(argv[index], argv[index + 1]).second)
                throw std::invalid_argument("Unknown, duplicate or incomplete argument; use --help");
        }
        if (!options.count(L"--config") || !options.count(L"--output") ||
            options.count(L"--image") + options.count(L"--manifest") != 1)
            throw std::invalid_argument("Require config, output and exactly one input");

        std::string split = "train";
        if (options.count(L"--evaluation-split")) {
            split = options.at(L"--evaluation-split").u8string();
            if (!options.count(L"--manifest") || (split != "train" && split != "validation" && split != "test"))
                throw std::invalid_argument("Evaluation requires a manifest and a valid split");
        }
        const auto& config = options.at(L"--config");
        const auto configBytes = read_bytes(config, config_file_limit);
        const PooledSmoothModel model(config.parent_path(), configBytes);
        std::vector<InputRecord> records;
        std::string manifestHash;
        if (options.count(L"--manifest")) {
            const auto& manifest = options.at(L"--manifest");
            const auto bytes = read_bytes(manifest, manifestLimit);
            manifestHash = sha256(bytes);
            records = load_manifest(manifest, bytes, split);
        } else {
            const auto image = fs::absolute(options.at(L"--image"));
            records.push_back({image, image.u8string(), "", "", ""});
        }

        const auto output = fs::absolute(options.at(L"--output"));
        if (fs::exists(output) || !fs::create_directories(output))
            throw std::runtime_error("Output must be a new directory");
        write_text(output / "config.json", std::string(configBytes.begin(), configBytes.end()));
        std::ofstream csv(output / "results.csv", std::ios::binary);
        csv.exceptions(std::ios::failbit | std::ios::badbit);
        csv << "source_path,split,label,image_sha256,config_sha256,width,height,score,decision,reason,load_ms,analysis_ms\n";
        std::map<InspectionOutcome, int> counts;
        for (const auto& record : records) {
            const auto start = Clock::now();
            std::string imageHash;
            std::string reason;
            cv::Mat image;
            auto result = AnalysisOutput{std::nullopt, InspectionOutcome::Error, "analysis", {}};
            double loadMs = 0;
            try {
                const auto bytes = read_bytes(record.file, image_file_limit);
                imageHash = sha256(bytes);
                if (!record.expectedHash.empty() && record.expectedHash != imageHash)
                    throw std::runtime_error("Image SHA-256 mismatch");
                image = decode_png(bytes);
                loadMs = elapsed(start);
                if (image.type() != CV_8UC3) throw std::runtime_error("Unsupported PNG pixel format");
                result = analyze_image(image, model);
                reason = result.reason;
            } catch (const std::exception& error) {
                loadMs = elapsed(start);
                reason = error.what();
            }
            ++counts[result.outcome];
            const std::string fields[] = {record.path, record.split, record.label, imageHash, PooledSmoothModel::configHash,
                image.empty() ? "" : std::to_string(image.cols), image.empty() ? "" : std::to_string(image.rows),
                result.score ? format_score(*result.score) : "", outcome_name(result.outcome), reason};
            for (const auto& field : fields) csv << csv_field(field) << ',';
            csv << std::setprecision(17) << loadMs << ','
                << std::chrono::duration<double, std::milli>(result.elapsed).count() << '\n';
        }
        csv.close();
        cv::FileStorage summary(".json", cv::FileStorage::WRITE | cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
        summary << "algorithm_version" << inspectionVersion << "opencv_version" << CV_VERSION
                << "config_sha256" << PooledSmoothModel::configHash << "manifest_sha256" << manifestHash
                << "requested" << static_cast<int>(records.size())
                << "PASS" << counts[InspectionOutcome::Pass] << "FAIL" << counts[InspectionOutcome::Fail]
                << "REVIEW" << counts[InspectionOutcome::Review] << "ERROR" << counts[InspectionOutcome::Error];
        write_text(output / "summary.json", summary.releaseAndGetString());
        std::cout << "Results: " << output.u8string() << '\n';
        return counts[InspectionOutcome::Error] ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
