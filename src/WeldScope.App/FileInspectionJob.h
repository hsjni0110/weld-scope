#pragma once

#include "AnalysisPipeline.h"
#include "InspectionInput.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace weldscope {
struct FileInspectionRequest {
    std::filesystem::path path;
    std::filesystem::path config;
    InspectionMode mode = InspectionMode::Baseline;
};

struct InspectedFile {
    std::filesystem::path path;
    std::string inputHash;
    cv::Mat original, overlay;
    InspectionOutcome outcome = InspectionOutcome::Error;
    std::optional<double> score;
    std::string reason;
    std::vector<InspectionAttempt> attempts;
    double loadMs{}, analysisMs{};
};

struct InspectionCancelled {};
using FileInspectionResult = std::variant<InspectedFile, FileInspectionFailure, InspectionCancelled>;

FileInspectionResult inspect_file(const FileInspectionRequest& request, const std::atomic_bool& cancelled);

struct InspectionEvidence {
    std::string sourceSha256;
    std::string configSha256;
};
FileInspectionResult inspect_file_with_evidence(const FileInspectionRequest& request, const std::atomic_bool& cancelled,
    InspectionEvidence& evidence, const std::vector<unsigned char>* fixedConfig = nullptr,
    std::string_view expectedSourceHash = {}, const std::function<void()>& beforePrecise = {});

enum class InspectionJobState { Empty, Ready, Running, Complete, Cancelled, Error, Closing };

// The owning UI thread selects, starts, cancels and polls the sole worker.
class FileInspectionJob {
public:
    using Runner = std::function<FileInspectionResult(const FileInspectionRequest&, const std::atomic_bool&)>;
    bool select(const std::filesystem::path& path);
    bool start(const std::filesystem::path& config, Runner run = inspect_file);
    void cancel();
    void request_close();
    std::optional<FileInspectionResult> poll();

    InspectionJobState state() const { return state_; }
    bool busy() const { return worker_.valid(); }
    const std::filesystem::path& selected_path() const { return selected_; }

private:
    struct Completion {
        std::uint64_t id;
        FileInspectionResult result;
    };

    std::filesystem::path selected_;
    InspectionJobState state_ = InspectionJobState::Empty;
    std::uint64_t nextId_ = 0;
    std::optional<std::uint64_t> acceptedId_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::future<Completion> worker_;
};
}
