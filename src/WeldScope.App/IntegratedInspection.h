#pragma once

#include "FileInspectionJob.h"
#include "InspectionHistory.h"
#include "Protocol.h"

#include <condition_variable>
#include <mutex>
#include <map>
#include <thread>

namespace weldscope {
enum class IntegrationState { Connecting, Disconnected, Ready, Inspecting, Saving, Stored, Unsaved, CommitUnknown, Conflict };

std::vector<std::filesystem::path> FindInspectionFiles(const std::filesystem::path& root);

struct BatchProgress {
    std::string batchId;
    unsigned int totalFiles = 0, savedFiles = 0;
    bool active = false;
    bool stopping = false;
};

struct IntegrationSnapshot {
    IntegrationState state = IntegrationState::Connecting;
    unsigned short port = 0;
    bool listenerFailed = false;
    std::optional<InspectionMode> fileMode;
    bool preciseRunning = false;
    std::optional<double> saveMs;
    std::uint64_t revision = 0;
    InspectionRecord record;
    std::optional<FileInspectionResult> result;
    std::optional<BatchProgress> batch;
    bool batchReady = false;
};

class IntegratedInspection {
public:
    IntegratedInspection(std::filesystem::path config, std::filesystem::path deviceImage, unsigned short port,
                         std::function<void()> beforeVerdict = {}, InspectionMode fileMode = InspectionMode::PooledSmooth);
    ~IntegratedInspection();
    IntegratedInspection(const IntegratedInspection&) = delete;
    IntegratedInspection& operator=(const IntegratedInspection&) = delete;

    bool StartFile(const std::filesystem::path& path);
    bool StartFolder(const std::filesystem::path& root, std::vector<std::filesystem::path> files);
    void CancelInspection();
    void Retry();
    void Pause(bool paused);
    void Close();
    bool Closed() const;
    IntegrationSnapshot Snapshot() const;

private:
    enum class Action { Idle, Connect, CreateBatch, Inspect, Save, FinishBatch };
    struct Job {
        FileInspectionRequest input;
        std::string sourceId, configId;
        std::string configSha256;
        std::vector<unsigned char> fixedConfig;
        bool commitUnconfirmed = false;
    };
    struct RegisteredConfig {
        std::string sha256;
        std::vector<unsigned char> bytes;
        std::shared_ptr<const PooledSmoothModel> model = {};
    };
    struct FolderWork {
        std::filesystem::path root;
        std::vector<std::filesystem::path> files;
        InspectionMode mode;
        RegisteredConfig config;
    };
    struct RequestLookup {
        std::string requestId;
        std::future<std::optional<StoredInspection>> result;
    };

    void RunWorker();
    void CreateBatchRecord();
    void QueueNextBatchStep();
    void FinishBatchRecord();
    void MarkBatchActionForRetry(Action action);
    void AnalyzeCurrentFile();
    void SaveCurrentInspection();
    void Connect();
    void SetWorkerAction(Action action, IntegrationState state);
    void QueueInspection(Job job, std::string requestId);
    bool HasActiveInspection() const;
    bool CanStartInspection() const;
    std::string Respond(const device::Request& request, RequestLookup& lookup);

    const std::filesystem::path config_, deviceImage_;
    const std::function<void()> beforeVerdict_;
    const InspectionMode fileMode_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool paused_ = false, closing_ = false, workerClosed_ = false, listenerClosed_ = false;
    std::atomic_bool stopListener_{false}, cancelled_{false};
    Action action_ = Action::Connect;
    std::optional<Job> job_;
    std::optional<FolderWork> folder_;
    std::optional<Action> folderRetry_;
    IntegrationSnapshot snapshot_;
    std::map<InspectionMode, RegisteredConfig> registered_;
    std::thread worker_, listener_;
};
}
