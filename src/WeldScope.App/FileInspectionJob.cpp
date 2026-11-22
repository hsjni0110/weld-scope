#include "FileInspectionJob.h"
#include "PooledSmoothModel.h"
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <utility>

namespace weldscope {
namespace {
using State = InspectionJobState;
}

FileInspectionResult inspect_file(const FileInspectionRequest& request, const std::atomic_bool& cancelled)
{
    InspectionEvidence evidence;
    return inspect_file_with_evidence(request, cancelled, evidence);
}

FileInspectionResult inspect_file_with_evidence(const FileInspectionRequest& request, const std::atomic_bool& cancelled,
    InspectionEvidence& evidence, std::string_view expectedSourceHash)
{
    if (cancelled) return InspectionCancelled{};
    auto failure = FileInspectionFailure::Configuration;
    try {
        auto model = request.model;
        if (!model) {
            const auto config = read_bytes(request.config, config_file_limit);
            evidence.configSha256 = sha256(config);
            model = std::make_shared<PooledSmoothModel>(request.config.parent_path(), config);
        }
        evidence.configSha256 = PooledSmoothModel::configHash;
        if (cancelled) return InspectionCancelled{};

        failure = FileInspectionFailure::Analysis;
        const auto bytes = read_bytes(request.path, image_file_limit);
        if (cancelled) return InspectionCancelled{};
        evidence.sourceSha256 = sha256(bytes);
        if (!expectedSourceHash.empty() && evidence.sourceSha256 != expectedSourceHash)
            return FileInspectionFailure::FileAccess;
        const auto bgr = decode_png(bytes);
        if (cancelled) return InspectionCancelled{};
        if (bgr.type() != CV_8UC3) return FileInspectionFailure::UnsupportedFormat;
        const auto result = analyze_image(bgr, *model);
        if (cancelled) return InspectionCancelled{};

        InspectedFile completed;
        completed.path = request.path;
        completed.inputHash = evidence.sourceSha256;
        completed.outcome = result.outcome;
        completed.score = result.score;
        completed.reason = result.reason;
        completed.analysisMs = std::chrono::duration<double, std::milli>(result.elapsed).count();
        cv::cvtColor(bgr, completed.original, cv::COLOR_BGR2BGRA);
        if (cancelled) return InspectionCancelled{};
        return completed;
    } catch (const InspectionInputError& error) {
        if (cancelled) return InspectionCancelled{};
        if (failure != FileInspectionFailure::Configuration) return error.reason();
        return failure;
    } catch (const std::exception&) {
        if (cancelled) return InspectionCancelled{};
        return failure;
    }
}

bool FileInspectionJob::select(const std::filesystem::path& path)
{
    if (state_ == State::Running || state_ == State::Closing || path.empty()) return false;
    selected_ = path;
    state_ = State::Ready;
    return true;
}

bool FileInspectionJob::try_start(const std::filesystem::path& config, Runner run)
{
    if (busy() || selected_.empty() || state_ == State::Closing) return false;
    try {
        const auto id = ++nextId_;
        auto token = std::make_shared<std::atomic_bool>(false);
        worker_ = std::async(std::launch::async, [id, request = FileInspectionRequest{selected_, config},
            token, run = std::move(run)] {
            try {
                return Completion{id, run(request, *token)};
            } catch (...) {
                return Completion{id, FileInspectionFailure::Analysis};
            }
        });
        cancelled_ = std::move(token);
        acceptedId_ = id;
        state_ = State::Running;
        return true;
    } catch (const std::exception&) {
        state_ = State::Error;
        return false;
    }
}

void FileInspectionJob::cancel()
{
    if (state_ != State::Running) return;
    acceptedId_.reset();
    cancelled_->store(true);
    state_ = State::Cancelled;
}

void FileInspectionJob::request_close()
{
    cancel();
    state_ = State::Closing;
}

std::optional<FileInspectionResult> FileInspectionJob::poll()
{
    if (!busy() || worker_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return std::nullopt;
    auto completion = worker_.get();
    cancelled_.reset();
    if (acceptedId_ != completion.id) return std::nullopt;
    acceptedId_.reset();
    if (std::holds_alternative<InspectedFile>(completion.result)) state_ = State::Complete;
    else if (std::holds_alternative<FileInspectionFailure>(completion.result)) state_ = State::Error;
    else state_ = State::Cancelled;
    return std::move(completion.result);
}
}
