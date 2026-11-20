#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "AnalysisPipeline.h"
#include "InspectionFiles.h"
#include "InspectionInput.h"
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
        record.debug = debug == 1;
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
InspectionMode parse_mode(const std::wstring& name)
{
    if (name == L"baseline") return InspectionMode::Baseline;
    if (name == L"fast") return InspectionMode::Fast;
    if (name == L"precise") return InspectionMode::Precise;
    if (name == L"selective") return InspectionMode::Selective;
    throw std::invalid_argument("Unknown algorithm; use --help");
}
fs::path executable_path()
{
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) throw std::runtime_error("Could not find current executable");
    path.resize(length);
    return fs::path(path);
}
void verify_freeze(const fs::path& file, const std::string& algorithm, const std::string& manifestHash,
    const std::string& configHash)
{
    const auto json = read_json(read_bytes(file, config_file_limit));
    if (read_integer(json["schema_version"], "schema_version") != 1)
        throw std::invalid_argument("Unsupported evaluation freeze schema");
    if (string_value(json["test_manifest_sha256"], "test_manifest_sha256") != manifestHash)
        throw std::invalid_argument("Test manifest SHA-256 differs from the evaluation freeze");
    const auto hashes = json["config_sha256"];
    for (const auto* key : {"baseline", "fast", "precise", "selective"}) {
        if (!is_sha256(string_value(hashes[key], key)))
            throw std::invalid_argument("Invalid configuration SHA-256 in the evaluation freeze");
    }
    if (string_value(hashes[algorithm], algorithm.c_str()) != configHash)
        throw std::invalid_argument("Configuration SHA-256 differs from the evaluation freeze");
    const auto executableHash = sha256(read_bytes(executable_path(), 256 * 1024 * 1024));
    if (string_value(json["executable_sha256"], "executable_sha256") != executableHash)
        throw std::invalid_argument("Executable SHA-256 differs from the evaluation freeze");
}
struct StageFields {
    std::string elapsed, score, outcome, reason;
};
StageFields stage_fields(const AnalysisOutput& result, const char* stage)
{
    for (const auto& attempt : result.attempts) {
        if (attempt.stage == stage) {
            return {std::to_string(attempt.elapsed.count()),
                    attempt.score ? format_score(*attempt.score) : "",
                    outcome_name(attempt.outcome), attempt.reason};
        }
    }
    return {};
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
        const std::set<std::wstring> allowed{L"--config", L"--output", L"--image", L"--manifest",
            L"--algorithm", L"--evaluation-split", L"--freeze"};
        if (argc == 2 && std::wstring(argv[1]) == L"--help") {
            std::cout << "weldscope_inspect [--algorithm baseline|fast|precise|selective] --config config.json "
                         "(--image file.png | --manifest manifest.json) --output NEW_DIRECTORY\n"
                         "Manifest evaluation: --evaluation-split validation|test (test requires --freeze freeze.json)\n";
            return 0;
        }
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc || !allowed.count(argv[index]) || !options.emplace(argv[index], argv[index + 1]).second)
                throw std::invalid_argument("Unknown, duplicate or incomplete argument; use --help");
        }
        if (!options.count(L"--config") || !options.count(L"--output")
            || options.count(L"--image") + options.count(L"--manifest") != 1)
            throw std::invalid_argument("Require config, output and exactly one input mode");
        std::string split = "train";
        if (const auto evaluation = options.find(L"--evaluation-split"); evaluation != options.end()) {
            split = evaluation->second.u8string();
            if (!options.count(L"--manifest") || (split != "validation" && split != "test"))
                throw std::invalid_argument("Evaluation requires a manifest and a validation or test split");
        }
        if (options.count(L"--freeze") != static_cast<std::size_t>(split == "test"))
            throw std::invalid_argument("Test evaluation requires --freeze; other modes reject it");
        const auto algorithm = options.count(L"--algorithm") ? options.at(L"--algorithm").wstring() : L"baseline";
        const auto mode = parse_mode(algorithm);
        const bool local = mode != InspectionMode::Baseline;
        const bool selective = mode == InspectionMode::Selective;
        const bool evaluation = split != "train";
        const bool reportStages = evaluation || selective;
        const auto configBytes = read_bytes(options.at(L"--config"), config_file_limit);
        const auto settings = parse_inspection_parameters(read_json(configBytes), mode);
        const auto configHash = sha256(configBytes);
        std::vector<InputRecord> records;
        std::string manifestHash;
        if (options.count(L"--manifest")) {
            const auto& manifest = options.at(L"--manifest");
            const auto manifestBytes = read_bytes(manifest, manifestLimit);
            if (evaluation) manifestHash = sha256(manifestBytes);
            if (split == "test") verify_freeze(options.at(L"--freeze"), fs::path(algorithm).u8string(),
                manifestHash, configHash);
            records = load_manifest(manifest, manifestBytes, split);
        } else {
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
        if (local) csv << ",continuous_candidate_ratio";
        if (reportStages)
            csv << ",fast_elapsed_us,precise_elapsed_us,fast_score,precise_score,reason,"
                   "fast_outcome,fast_reason,precise_outcome,precise_reason";
        csv << '\n';
        int passed = 0, failed = 0, errors = 0, reviewed = 0;
        bool inputManifestValid = true;
        std::size_t ordinal = 0;
        for (const auto& record : records) {
            ++ordinal;
            std::string imageHash, decision = "ERROR", errorText, artifactPath;
            std::string width, height, candidates, total, score, continuousCandidateRatio;
            double loadMs = 0, analysisMs = 0, artifactMs = 0;
            cv::Mat image;
            AnalysisOutput result{};
            bool hashVerified = false;
            const auto loadStart = Clock::now();
            try {
                const auto bytes = read_bytes(record.file, image_file_limit);
                imageHash = sha256(bytes);
                if (!record.expectedHash.empty() && record.expectedHash != imageHash) throw std::runtime_error("Image SHA-256 mismatch");
                hashVerified = true;
                image = decode_png(bytes);
                loadMs = elapsed(loadStart);
                result = analyze_image(image, settings);
                analysisMs = result.analysisMs;
                width = std::to_string(image.cols);
                height = std::to_string(image.rows);
                candidates = std::to_string(result.candidatePixels);
                total = std::to_string(result.totalPixels);
                if (result.score) score = format_score(*result.score);
                if (result.continuousCandidateRatio)
                    continuousCandidateRatio = format_score(*result.continuousCandidateRatio);
                decision = outcome_name(result.outcome);
                if (result.outcome == InspectionOutcome::Error) errorText = result.reason;
            } catch (const std::exception& error) {
                errorText = error.what();
                if (evaluation && !hashVerified) inputManifestValid = false;
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
            else if (decision == "REVIEW") ++reviewed;
            else ++errors;
            const std::string fields[] = {record.path, record.split, record.label, imageHash, configHash, width, height,
                candidates, total, score, decision, errorText};
            for (const auto& field : fields) csv << csv_field(field) << ',';
            csv << std::setprecision(17) << loadMs << ',' << analysisMs << ',' << artifactMs << ',' << csv_field(artifactPath);
            if (local) csv << ',' << continuousCandidateRatio;
            if (reportStages) {
                const auto fast = stage_fields(result, "FAST");
                const auto precise = stage_fields(result, "PRECISE");
                const std::string observations[] = {fast.elapsed, precise.elapsed, fast.score, precise.score,
                    result.reason.empty() ? errorText : result.reason, fast.outcome, fast.reason, precise.outcome, precise.reason};
                for (const auto& field : observations) csv << ',' << csv_field(field);
            }
            csv << '\n';
        }
        csv.close();
        cv::FileStorage summary(".json", cv::FileStorage::WRITE | cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
        const char* scope = options.count(L"--manifest") ? "train-development-only" : "unlabelled-single-image";
        if (evaluation) scope = split == "validation" ? "validation-policy" : "test-frozen";
        summary << "algorithm_version" << inspection_version(mode) << "opencv_version" << CV_VERSION
                << "scope" << scope << "independent_evaluation" << 0
                << "requested" << static_cast<int>(records.size()) << "PASS" << passed << "FAIL" << failed << "ERROR" << errors
                << "threads" << cv::getNumThreads() << "config_sha256" << configHash
                << (local ? "contrast_threshold" : "intensity_threshold") << settings.threshold;
        if (!selective) summary << "fail_ratio" << settings.failRatio;
        if (reportStages) summary << "REVIEW" << reviewed;
        if (evaluation)
            summary << "evaluation_split" << split << "manifest_sha256" << manifestHash
                    << "input_manifest_valid" << static_cast<int>(inputManifestValid);
        write_text(output / "summary.json", summary.releaseAndGetString());
        std::cout << "Requested " << records.size() << ", PASS " << passed << ", FAIL " << failed << ", ERROR " << errors;
        if (reportStages) std::cout << ", REVIEW " << reviewed;
        std::cout << '\n' << "Results: " << output.u8string() << '\n';
        return errors ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
