#include "IntegratedInspection.h"
#include "LoopbackServer.h"

#include <rpc.h>

#include <chrono>
#include <utility>

namespace weldscope {
namespace {
constexpr auto sourceId = "weld-01";
constexpr auto configId = "baseline-v1";
constexpr auto sourceHash = "4a08fa2c6274e603f18358134092e8f697cc81f78a36f472577bca81bc0de3f3";
constexpr auto configHash = "266b8fbb2b4463ba2423126bc9619d45831e7e151862f73f7aaaa88f892bedb2";
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

std::string StoredResponse(const InspectionRecord& record) {
    if (record.workState == InspectionWorkState::Cancelled)
        return device::StatusFrame(record.requestId, record.inspectionId, "CANCELLED");
    return device::ResultFrame(record.requestId, record.inspectionId, record.decision);
}
}

IntegratedInspection::IntegratedInspection(std::filesystem::path config, std::filesystem::path deviceImage,
    unsigned short port, std::function<void()> beforeVerdict)
    : config_(std::move(config)), deviceImage_(std::move(deviceImage)), beforeVerdict_(std::move(beforeVerdict)) {
    worker_ = std::thread([this] { Run(); });
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

void IntegratedInspection::Queue(Action action, IntegrationState state) {
    action_ = action;
    snapshot_.state = state;
    ++snapshot_.revision;
    changed_.notify_one();
}

void IntegratedInspection::Begin(Job job, std::string requestId) {
    InspectionRecord record;
    record.inspectionId = NewId();
    record.requestId = std::move(requestId);
    record.inputId = job.sourceId.empty() ? record.requestId : job.sourceId;
    record.sourcePath = job.input.path.u8string();
    record.configSha256 = job.configId.empty() ? "" : configHash;
    record.algorithmVersion = "global-dark-ratio-v1";
    record.datasetVersion = job.sourceId.empty() ? "local-file" : "openzdm-14025630";
    snapshot_.record = std::move(record);
    snapshot_.result.reset();
    job_ = std::move(job);
    cancelled_ = false;
    Queue(Action::Inspect, IntegrationState::Inspecting);
}

bool IntegratedInspection::CanStart() const {
    return !paused_ && !closing_ &&
           (snapshot_.state == IntegrationState::Ready || snapshot_.state == IntegrationState::Stored);
}

bool IntegratedInspection::StartFile(const std::filesystem::path& path) {
    const std::lock_guard lock(mutex_);
    if (!CanStart() || path.empty()) return false;
    Begin({{path, config_}, {}, {}}, "file-" + NewId());
    return true;
}

void IntegratedInspection::CancelInspection() {
    const std::lock_guard lock(mutex_);
    if (snapshot_.state == IntegrationState::Inspecting) cancelled_ = true;
}

void IntegratedInspection::Retry() {
    const std::lock_guard lock(mutex_);
    if (closing_) return;
    if (snapshot_.state == IntegrationState::Disconnected) Queue(Action::Connect, IntegrationState::Connecting);
    if (HasPendingSave(snapshot_.state)) Queue(Action::Save, IntegrationState::Saving);
}

void IntegratedInspection::Pause(bool paused) {
    const std::lock_guard lock(mutex_);
    paused_ = paused;
}

void IntegratedInspection::Close() {
    const std::lock_guard lock(mutex_);
    closing_ = true;
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

void IntegratedInspection::Run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        changed_.wait(lock, [this] { return closing_ || action_ != Action::Idle; });
        const auto action = std::exchange(action_, Action::Idle);
        if (closing_ && action == Action::Idle) break;
        lock.unlock();
        if (action == Action::Connect) Connect();
        else if (action == Action::Inspect) Inspect();
        else if (action == Action::Save) Save();
        lock.lock();
    }
    workerClosed_ = true;
}

void IntegratedInspection::Connect() {
    auto state = IntegrationState::Disconnected;
    std::vector<unsigned char> config;
    try {
        InspectionHistory(HistoryConnectionFromEnvironment()).CheckReady();
        state = IntegrationState::Ready;
    } catch (const std::exception&) {}
    if (state == IntegrationState::Ready) {
        try {
            config = read_bytes(config_, config_file_limit);
            if (sha256(config) != configHash) config.clear();
        } catch (const std::exception&) { config.clear(); }
    }
    const std::lock_guard lock(mutex_);
    deviceConfig_ = std::move(config);
    Queue(Action::Idle, state);
}

void IntegratedInspection::Inspect() {
    InspectionEvidence evidence;
    FileInspectionResult result = FileInspectionFailure::Analysis;
    try {
        const auto* fixtureConfig = job_->configId.empty() ? nullptr : &deviceConfig_;
        result = inspect_file_with_evidence(job_->input, cancelled_, evidence, fixtureConfig,
            fixtureConfig ? sourceHash : "");
        if (beforeVerdict_) beforeVerdict_();
    } catch (const std::exception&) { result = FileInspectionFailure::Analysis; }

    const std::lock_guard lock(mutex_);
    if (cancelled_) result = InspectionCancelled{};
    auto& record = snapshot_.record;
    record.sourceSha256 = std::move(evidence.sourceSha256);
    if (job_->sourceId.empty() && !record.sourceSha256.empty()) record.inputId = "file:" + record.sourceSha256;
    if (!evidence.configSha256.empty()) record.configSha256 = std::move(evidence.configSha256);
    if (const auto* inspected = std::get_if<InspectedFile>(&result)) {
        record.decision = inspected->decision == BaselineDecision::Pass ? "PASS" : "FAIL";
        record.reason = "dark_pixel_ratio";
        record.attempts.push_back({"BASELINE", inspected->score, record.decision, record.reason, inspected->analysisMs});
    } else if (const auto* error = std::get_if<FileInspectionFailure>(&result)) {
        record.decision = "ERROR";
        record.reason = FailureReason(*error);
    } else {
        record.workState = InspectionWorkState::Cancelled;
        record.reason = "cancelled";
    }
    snapshot_.result = std::move(result);
    Queue(Action::Save, IntegrationState::Saving);
}

void IntegratedInspection::Save() {
    auto state = job_->commitUnconfirmed ? IntegrationState::CommitUnknown : IntegrationState::Unsaved;
    try {
        const auto saved = InspectionHistory(HistoryConnectionFromEnvironment()).Save(snapshot_.record);
        state = saved == SaveStatus::Conflict ? IntegrationState::Conflict : IntegrationState::Stored;
    } catch (const CommitOutcomeUnknown&) {
        state = IntegrationState::CommitUnknown;
    } catch (const std::exception&) {}
    const std::lock_guard lock(mutex_);
    if (state == IntegrationState::Stored) job_.reset();
    else if (state == IntegrationState::CommitUnknown) job_->commitUnconfirmed = true;
    Queue(Action::Idle, state);
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
            Queue(Action::Save, IntegrationState::Saving);
            return StatusFrame(request.requestId, id, state == IntegrationState::CommitUnknown ? "COMMIT_UNKNOWN" : "UNSAVED");
        }
        wasBusy = snapshot_.state == IntegrationState::Inspecting || snapshot_.state == IntegrationState::Saving;
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
            if (existing->record.inputId != request.sourceId || request.configId != configId ||
                existing->record.configSha256 != configHash)
                return ErrorFrame(request.requestId, "REQUEST_CONFLICT");
            return StoredResponse(existing->record);
        }
        const std::lock_guard lock(mutex_);
        if (wasBusy || snapshot_.state == IntegrationState::Inspecting || snapshot_.state == IntegrationState::Saving)
            return ErrorFrame(request.requestId, "BUSY");
        if (!CanStart()) return ErrorFrame(request.requestId, "DB_UNAVAILABLE");
        if (request.sourceId != sourceId) return ErrorFrame(request.requestId, "UNKNOWN_SOURCE");
        if (request.configId != configId) return ErrorFrame(request.requestId, "UNKNOWN_CONFIG");
        if (deviceConfig_.empty()) return ErrorFrame(request.requestId, "CONFIG_UNAVAILABLE");
        Begin({{deviceImage_, config_}, std::string(request.sourceId), std::string(request.configId)}, std::string(request.requestId));
        return StatusFrame(request.requestId, snapshot_.record.inspectionId, "IN_PROGRESS");
    } catch (const std::exception&) {
        const std::lock_guard lock(mutex_);
        if (wasBusy || snapshot_.state == IntegrationState::Inspecting || snapshot_.state == IntegrationState::Saving)
            return ErrorFrame(request.requestId, "BUSY");
        if (CanStart()) Queue(Action::Idle, IntegrationState::Disconnected);
        return ErrorFrame(request.requestId, "DB_UNAVAILABLE");
    }
}
}
