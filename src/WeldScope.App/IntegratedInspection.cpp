#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "IntegratedInspection.h"
#include "LoopbackServer.h"

#include <rpc.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace weldscope {
namespace {
constexpr auto sourceId = "weld-01";
constexpr auto baselineId = "baseline-v1";
constexpr auto selectiveId = "selective-v1";
constexpr auto sourceHash = "4a08fa2c6274e603f18358134092e8f697cc81f78a36f472577bca81bc0de3f3";
constexpr auto baselineHash = "266b8fbb2b4463ba2423126bc9619d45831e7e151862f73f7aaaa88f892bedb2";
constexpr auto selectiveHash = "65db00364fbd04baae490e602881bfcd97307d2449f3f6b3e8eb5b17f4d1ac93";
constexpr auto lookupBudget = std::chrono::milliseconds(150);

std::string NewId() {
    UUID uuid{};
    const auto status = UuidCreate(&uuid);
    if (status != RPC_S_OK && status != RPC_S_UUID_LOCAL_ONLY)
        throw std::runtime_error("Could not generate inspection identifier");
    RPC_CSTR text = nullptr;
    if (UuidToStringA(&uuid, &text) != RPC_S_OK)
        throw std::runtime_error("Could not format inspection identifier");
    const auto freeText = [](unsigned char* value) { RpcStringFreeA(&value); };
    const std::unique_ptr<unsigned char, decltype(freeText)> owner(text, freeText);
    return reinterpret_cast<const char*>(owner.get());
}

bool HasPendingSave(IntegrationState state) {
    return state == IntegrationState::Unsaved || state == IntegrationState::CommitUnknown ||
           state == IntegrationState::Conflict;
}

const char* FailureReason(FileInspectionFailure failure) {
    switch (failure) {
    case FileInspectionFailure::Configuration: return "configuration";
    case FileInspectionFailure::FileAccess: return "file_access";
    case FileInspectionFailure::SizeLimit: return "size_limit";
    case FileInspectionFailure::DamagedPng: return "damaged_png";
    case FileInspectionFailure::UnsupportedFormat: return "unsupported_format";
    case FileInspectionFailure::Analysis: return "analysis";
    }
    return "analysis";
}

void RecordAnalysisOutcome(InspectionRecord& record, const FileInspectionResult& result) {
    if (const auto* inspected = std::get_if<InspectedFile>(&result)) {
        record.decision = outcome_name(inspected->outcome);
        record.reason = inspected->reason;
        for (const auto& attempt : inspected->attempts) {
            const auto durationMs = std::chrono::duration<double, std::milli>(attempt.elapsed).count();
            record.attempts.push_back({attempt.stage, attempt.score, outcome_name(attempt.outcome),
                attempt.reason, durationMs});
        }
    } else if (const auto* failure = std::get_if<FileInspectionFailure>(&result)) {
        record.decision = "ERROR";
        record.reason = FailureReason(*failure);
    } else {
        record.workState = InspectionWorkState::Cancelled;
        record.reason = "cancelled";
    }
}

std::string StoredResponse(const InspectionRecord& record) {
    if (record.workState == InspectionWorkState::Cancelled)
        return device::StatusFrame(record.requestId, record.inspectionId, "CANCELLED");
    return device::ResultFrame(record.requestId, record.inspectionId, record.decision);
}

DWORD FileAttributes(const std::filesystem::path& path) {
    constexpr DWORD reparsePoint = FILE_ATTRIBUTE_REPARSE_POINT;
    const auto flags = GetFileAttributesW(path.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES) {
        const auto error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        throw std::filesystem::filesystem_error("Could not read inspection path", path, error);
    }
    return flags & (FILE_ATTRIBUTE_DIRECTORY | reparsePoint);
}
}

std::vector<std::filesystem::path> FindInspectionFiles(const std::filesystem::path& root) {
    const auto rootFlags = FileAttributes(root);
    if (rootFlags != FILE_ATTRIBUTE_DIRECTORY)
        throw std::filesystem::filesystem_error("Expected a regular inspection folder", root,
            std::make_error_code(std::errc::not_a_directory));
    std::vector<std::filesystem::path> files;
    const std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator entry(root); entry != end; ++entry) {
        const auto& path = entry->path();
        const auto flags = FileAttributes(path);
        if ((flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            entry.disable_recursion_pending();
        } else if (flags == 0) {
            const auto extension = path.extension();
            if (CompareStringOrdinal(extension.c_str(), -1, L".png", -1, TRUE) == CSTR_EQUAL)
                files.push_back(path.lexically_relative(root));
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

IntegratedInspection::IntegratedInspection(std::filesystem::path config, std::filesystem::path deviceImage,
    unsigned short port, std::function<void()> beforeVerdict)
    : config_(std::move(config)), deviceImage_(std::move(deviceImage)), beforeVerdict_(std::move(beforeVerdict)) {
    worker_ = std::thread([this] { RunWorker(); });
    try {
        listener_ = std::thread([this, port] {
            try {
                RequestLookup lookup;
                device::ServeLoopback(port, stopListener_,
                    [this, &lookup](const device::Request& request) { return Respond(request, lookup); },
                    [this](unsigned short listeningPort) {
                        const std::lock_guard lock(mutex_);
                        snapshot_.port = listeningPort;
                        ++snapshot_.revision;
                    });
            } catch (const std::exception&) {
                const std::lock_guard lock(mutex_);
                snapshot_.listenerFailed = true;
                snapshot_.port = 0;
                ++snapshot_.revision;
            }
            const std::lock_guard lock(mutex_);
            listenerClosed_ = true;
        });
    } catch (...) {
        Close();
        worker_.join();
        throw;
    }
}

IntegratedInspection::~IntegratedInspection() {
    Close();
    worker_.join();
    listener_.join();
}

void IntegratedInspection::SetWorkerAction(Action action, IntegrationState state) {
    action_ = action;
    snapshot_.state = state;
    ++snapshot_.revision;
    changed_.notify_one();
}

void IntegratedInspection::QueueInspection(Job job, std::string requestId) {
    InspectionRecord record;
    record.inspectionId = NewId();
    record.requestId = std::move(requestId);
    record.inputId = job.sourceId.empty() ? record.requestId : job.sourceId;
    record.sourcePath = job.input.path.u8string();
    record.configSha256 = job.configSha256;
    record.algorithmVersion = inspection_version(job.input.mode);
    record.datasetVersion = job.sourceId.empty() ? "local-file" : "openzdm-14025630";
    if (folder_) {
        record.batchId = snapshot_.batch->batchId;
        record.batchIndex = snapshot_.batch->savedFiles + 1;
    }
    snapshot_.record = std::move(record);
    snapshot_.result.reset();
    snapshot_.preciseRunning = false;
    snapshot_.saveMs.reset();
    job_ = std::move(job);
    cancelled_ = false;
    SetWorkerAction(Action::Inspect, IntegrationState::Inspecting);
}

bool IntegratedInspection::CanStartInspection() const {
    return !paused_ && !closing_ && !folder_ &&
           (snapshot_.state == IntegrationState::Ready || snapshot_.state == IntegrationState::Stored);
}

bool IntegratedInspection::HasActiveInspection() const {
    return folder_ || snapshot_.state == IntegrationState::Inspecting || snapshot_.state == IntegrationState::Saving;
}

bool IntegratedInspection::StartFile(const std::filesystem::path& path) {
    const std::lock_guard lock(mutex_);
    if (!CanStartInspection() || path.empty() || !snapshot_.fileMode) return false;
    const auto mode = *snapshot_.fileMode;
    if (mode == InspectionMode::Selective) {
        QueueInspection({{path, {}, mode}, {}, {}, selectiveConfig_.sha256, selectiveConfig_.bytes}, "file-" + NewId());
    } else {
        QueueInspection({{path, config_, mode}, {}, {}, {}, {}}, "file-" + NewId());
    }
    return true;
}

bool IntegratedInspection::StartFolder(const std::filesystem::path& root, std::vector<std::filesystem::path> files) {
    if (!root.is_absolute() || files.empty() || files.size() > std::numeric_limits<unsigned int>::max() ||
        !std::is_sorted(files.begin(), files.end()) || std::adjacent_find(files.begin(), files.end()) != files.end() ||
        std::any_of(files.begin(), files.end(), [](const auto& path) {
            return path.empty() || path.has_root_path() ||
                   std::any_of(path.begin(), path.end(), [](const auto& part) { return part == L".." || part == L"."; });
        })) return false;

    const std::lock_guard lock(mutex_);
    if (!CanStartInspection() || !snapshot_.fileMode || !snapshot_.batchReady) return false;
    auto config = *snapshot_.fileMode == InspectionMode::Selective ? selectiveConfig_ : RegisteredConfig{};
    auto progress = BatchProgress{NewId(), static_cast<unsigned int>(files.size()), 0, true};
    folder_ = FolderWork{root, std::move(files), *snapshot_.fileMode, std::move(config)};
    snapshot_.batch = std::move(progress);
    snapshot_.record = {};
    snapshot_.result.reset();
    snapshot_.preciseRunning = false;
    snapshot_.saveMs.reset();
    SetWorkerAction(Action::CreateBatch, IntegrationState::Saving);
    return true;
}

void IntegratedInspection::CancelInspection() {
    const std::lock_guard lock(mutex_);
    if (folder_ && !snapshot_.batch->stopping) {
        snapshot_.batch->stopping = true;
        ++snapshot_.revision;
    }
    if (snapshot_.state == IntegrationState::Inspecting) cancelled_ = true;
}

void IntegratedInspection::Retry() {
    const std::lock_guard lock(mutex_);
    if (closing_) return;
    if (snapshot_.state == IntegrationState::Disconnected) SetWorkerAction(Action::Connect, IntegrationState::Connecting);
    if (HasPendingSave(snapshot_.state)) SetWorkerAction(folderRetry_.value_or(Action::Save), IntegrationState::Saving);
}

void IntegratedInspection::Pause(bool paused) {
    const std::lock_guard lock(mutex_);
    paused_ = paused;
}

void IntegratedInspection::Close() {
    const std::lock_guard lock(mutex_);
    closing_ = true;
    if (folder_) snapshot_.batch->stopping = true;
    if (snapshot_.state == IntegrationState::Inspecting) cancelled_ = true;
    stopListener_ = true;
    changed_.notify_one();
}

bool IntegratedInspection::Closed() const {
    const std::lock_guard lock(mutex_);
    return workerClosed_ && listenerClosed_;
}

IntegrationSnapshot IntegratedInspection::Snapshot() const {
    const std::lock_guard lock(mutex_);
    return snapshot_;
}

void IntegratedInspection::RunWorker() {
    std::unique_lock lock(mutex_);
    for (;;) {
        changed_.wait(lock, [this] { return closing_ || action_ != Action::Idle; });
        const auto action = std::exchange(action_, Action::Idle);
        if (closing_ && action == Action::Idle) break;
        lock.unlock();
        if (action == Action::Connect) Connect();
        else if (action == Action::CreateBatch) CreateBatchRecord();
        else if (action == Action::Inspect) AnalyzeCurrentFile();
        else if (action == Action::Save) SaveCurrentInspection();
        else if (action == Action::FinishBatch) FinishBatchRecord();
        lock.lock();
    }
    workerClosed_ = true;
}

void IntegratedInspection::Connect() {
    auto state = IntegrationState::Disconnected;
    RegisteredConfig baseline{baselineHash, {}}, selective;
    std::optional<InspectionMode> fileMode = InspectionMode::Baseline;
    bool batchReady = false;
    try {
        const InspectionHistory history(HistoryConnectionFromEnvironment());
        history.CheckReady();
        state = IntegrationState::Ready;
        history.CheckBatchReady();
        batchReady = true;
    } catch (const std::exception&) {}
    if (state == IntegrationState::Ready) {
        try {
            auto bytes = read_bytes(config_, config_file_limit);
            if (sha256(bytes) == baseline.sha256) baseline.bytes = std::move(bytes);
        } catch (const std::exception&) {}
        try {
            const auto folder = config_.parent_path();
            const auto settingsPath = folder / L"selective.json";
            const auto freezePath = folder / L"freeze.json";
#ifdef WELDSCOPE_S11_DEPLOYED
            const bool selectiveRequested = true;
#else
            const bool selectiveRequested = std::filesystem::exists(settingsPath) || std::filesystem::exists(freezePath);
#endif
            if (selectiveRequested) {
                const auto frozen = read_json(read_bytes(freezePath, config_file_limit));
                if (read_integer(frozen["schema_version"], "schema_version") != 1)
                    throw std::invalid_argument("Unsupported selective freeze schema");
                selective.sha256 = frozen["config_sha256"]["selective"].string();
                auto bytes = read_bytes(settingsPath, config_file_limit);
                if (sha256(bytes) != selective.sha256) throw std::invalid_argument("Selective settings hash mismatch");
                parse_inspection_parameters(read_json(bytes), InspectionMode::Selective);
                selective.bytes = std::move(bytes);
                fileMode = InspectionMode::Selective;
            }
        } catch (const std::exception&) { fileMode.reset(); }
    }
    const std::lock_guard lock(mutex_);
    baselineConfig_ = std::move(baseline);
    selectiveConfig_ = std::move(selective);
    snapshot_.fileMode = fileMode;
    snapshot_.batchReady = batchReady;
    SetWorkerAction(Action::Idle, state);
}

void IntegratedInspection::CreateBatchRecord() {
    try {
        auto& folder = *folder_;
        if (folder.config.bytes.empty()) {
            auto bytes = read_bytes(config_, config_file_limit);
            parse_inspection_parameters(read_json(bytes), folder.mode);
            folder.config = {sha256(bytes), std::move(bytes)};
        }
        InspectionHistory(HistoryConnectionFromEnvironment()).CreateBatch(snapshot_.batch->batchId,
            folder.root.u8string(), folder.config.sha256, snapshot_.batch->totalFiles);
    } catch (const std::exception&) {
        MarkBatchActionForRetry(Action::CreateBatch);
        return;
    }

    const std::lock_guard lock(mutex_);
    folderRetry_.reset();
    QueueNextBatchStep();
}

void IntegratedInspection::QueueNextBatchStep() {
    const auto& progress = *snapshot_.batch;
    if (progress.stopping || progress.savedFiles == progress.totalFiles) {
        SetWorkerAction(Action::FinishBatch, IntegrationState::Saving);
        return;
    }

    const auto& folder = *folder_;
    QueueInspection({{folder.root / folder.files[progress.savedFiles], {}, folder.mode}, {}, {}, folder.config.sha256,
        folder.config.bytes}, "file-" + progress.batchId + "-" + std::to_string(progress.savedFiles + 1));
}

void IntegratedInspection::MarkBatchActionForRetry(Action action) {
    const std::lock_guard lock(mutex_);
    folderRetry_ = action;
    SetWorkerAction(Action::Idle, IntegrationState::Unsaved);
}

void IntegratedInspection::FinishBatchRecord() {
    const auto& progress = *snapshot_.batch;
    const bool completed = progress.savedFiles == progress.totalFiles &&
        snapshot_.record.workState != InspectionWorkState::Cancelled;
    try {
        InspectionHistory(HistoryConnectionFromEnvironment()).FinishBatch(progress.batchId, completed ? "completed" : "stopped");
    } catch (const std::exception&) {
        MarkBatchActionForRetry(Action::FinishBatch);
        return;
    }

    const std::lock_guard lock(mutex_);
    folderRetry_.reset();
    folder_.reset();
    snapshot_.batch->active = false;
    SetWorkerAction(Action::Idle, IntegrationState::Stored);
}

void IntegratedInspection::AnalyzeCurrentFile() {
    InspectionEvidence evidence;
    FileInspectionResult result = FileInspectionFailure::Analysis;
    try {
        const auto* fixedConfig = job_->fixedConfig.empty() ? nullptr : &job_->fixedConfig;
        result = inspect_file_with_evidence(job_->input, cancelled_, evidence, fixedConfig,
            job_->sourceId.empty() ? "" : sourceHash, [this] {
                const std::lock_guard lock(mutex_);
                snapshot_.preciseRunning = true;
                ++snapshot_.revision;
            });
        if (beforeVerdict_) beforeVerdict_();
    } catch (const std::exception&) { result = FileInspectionFailure::Analysis; }

    const std::lock_guard lock(mutex_);
    if (cancelled_) result = InspectionCancelled{};
    auto& record = snapshot_.record;
    record.sourceSha256 = std::move(evidence.sourceSha256);
    if (job_->sourceId.empty() && !record.sourceSha256.empty()) record.inputId = "file:" + record.sourceSha256;
    if (!evidence.configSha256.empty()) record.configSha256 = std::move(evidence.configSha256);
    RecordAnalysisOutcome(record, result);
    snapshot_.result = std::move(result);
    SetWorkerAction(Action::Save, IntegrationState::Saving);
}

void IntegratedInspection::SaveCurrentInspection() {
    auto state = job_->commitUnconfirmed ? IntegrationState::CommitUnknown : IntegrationState::Unsaved;
    const auto start = std::chrono::steady_clock::now();
    try {
        const auto saved = InspectionHistory(HistoryConnectionFromEnvironment()).Save(snapshot_.record);
        state = saved == SaveStatus::Conflict ? IntegrationState::Conflict : IntegrationState::Stored;
    } catch (const CommitOutcomeUnknown&) {
        state = IntegrationState::CommitUnknown;
    } catch (const std::exception&) {}
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    const std::lock_guard lock(mutex_);
    snapshot_.saveMs = elapsed.count();
    if (state == IntegrationState::Stored) {
        job_.reset();
        if (folder_) {
            ++snapshot_.batch->savedFiles;
            QueueNextBatchStep();
            return;
        }
    } else if (state == IntegrationState::CommitUnknown) {
        job_->commitUnconfirmed = true;
    }
    SetWorkerAction(Action::Idle, state);
}

std::string IntegratedInspection::Respond(const device::Request& request, RequestLookup& lookup) {
    using namespace device;
    if (request.requestId.substr(0, 5) == "file-") return ErrorFrame(request.requestId, "RESERVED_REQUEST_ID");
    bool wasBusy = false;
    {
        const std::lock_guard lock(mutex_);
        if (closing_ || snapshot_.state == IntegrationState::Connecting ||
            snapshot_.state == IntegrationState::Disconnected)
            return ErrorFrame(request.requestId, "DB_UNAVAILABLE");
        if (job_ && snapshot_.record.requestId == request.requestId) {
            if (job_->sourceId != request.sourceId || job_->configId != request.configId)
                return ErrorFrame(request.requestId, "REQUEST_CONFLICT");
            const auto state = snapshot_.state;
            const auto& id = snapshot_.record.inspectionId;
            if (state == IntegrationState::Conflict) return ErrorFrame(request.requestId, "REQUEST_CONFLICT");
            if (!HasPendingSave(state)) return StatusFrame(request.requestId, id, "IN_PROGRESS");
            SetWorkerAction(Action::Save, IntegrationState::Saving);
            return StatusFrame(request.requestId, id, state == IntegrationState::CommitUnknown ? "COMMIT_UNKNOWN" : "UNSAVED");
        }
        wasBusy = HasActiveInspection();
    }

    try {
        // Keep the single listener responsive while one replay lookup is outstanding.
        if (lookup.result.valid() && lookup.requestId != request.requestId) {
            if (lookup.result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                return ErrorFrame(request.requestId, "BUSY");
            lookup.result = {};
        }
        if (!lookup.result.valid()) {
            lookup.requestId = request.requestId;
            lookup.result = std::async(std::launch::async, [id = lookup.requestId] {
                return InspectionHistory(HistoryConnectionFromEnvironment()).GetByRequestId(id);
            });
        }
        if (lookup.result.wait_for(lookupBudget) != std::future_status::ready)
            return ErrorFrame(request.requestId, "BUSY");
        const auto existing = lookup.result.get();
        if (existing) {
            const std::string_view hash = request.configId == baselineId ? baselineHash :
                request.configId == selectiveId ? selectiveHash : "";
            if (hash.empty() || existing->record.inputId != request.sourceId || existing->record.configSha256 != hash)
                return ErrorFrame(request.requestId, "REQUEST_CONFLICT");
            return StoredResponse(existing->record);
        }
        const std::lock_guard lock(mutex_);
        if (wasBusy || HasActiveInspection())
            return ErrorFrame(request.requestId, "BUSY");
        if (!CanStartInspection()) return ErrorFrame(request.requestId, "DB_UNAVAILABLE");
        if (request.sourceId != sourceId) return ErrorFrame(request.requestId, "UNKNOWN_SOURCE");
        const bool isBaseline = request.configId == baselineId;
        if (!isBaseline && request.configId != selectiveId) return ErrorFrame(request.requestId, "UNKNOWN_CONFIG");
        const auto& settings = isBaseline ? baselineConfig_ : selectiveConfig_;
        const auto expectedHash = isBaseline ? baselineHash : selectiveHash;
        if (settings.bytes.empty() || settings.sha256 != expectedHash)
            return ErrorFrame(request.requestId, "CONFIG_UNAVAILABLE");
        const auto mode = isBaseline ? InspectionMode::Baseline : InspectionMode::Selective;
        QueueInspection({{deviceImage_, {}, mode}, std::string(request.sourceId), std::string(request.configId),
            settings.sha256, settings.bytes}, std::string(request.requestId));
        return StatusFrame(request.requestId, snapshot_.record.inspectionId, "IN_PROGRESS");
    } catch (const std::exception&) {
        const std::lock_guard lock(mutex_);
        if (wasBusy || HasActiveInspection())
            return ErrorFrame(request.requestId, "BUSY");
        if (CanStartInspection()) SetWorkerAction(Action::Idle, IntegrationState::Disconnected);
        return ErrorFrame(request.requestId, "DB_UNAVAILABLE");
    }
}
}
