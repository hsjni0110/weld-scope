#pragma once

#include "FileInspectionJob.h"
#include "InspectionHistory.h"
#include "Protocol.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace weldscope {
enum class IntegrationState { Connecting, Disconnected, Ready, Inspecting, Saving, Stored, Unsaved, CommitUnknown, Conflict };

struct IntegrationSnapshot {
    IntegrationState state = IntegrationState::Connecting;
    unsigned short port = 0;
    bool listenerFailed = false;
    std::uint64_t revision = 0;
    InspectionRecord record;
    std::optional<FileInspectionResult> result;
};

class IntegratedInspection {
public:
    IntegratedInspection(std::filesystem::path config, std::filesystem::path deviceImage, unsigned short port,
                         std::function<void()> beforeVerdict = {});
    ~IntegratedInspection();
    IntegratedInspection(const IntegratedInspection&) = delete;
    IntegratedInspection& operator=(const IntegratedInspection&) = delete;

    bool StartFile(const std::filesystem::path& path);
    void CancelInspection();
    void Retry();
    void Pause(bool paused);
    void Close();
    bool Closed() const;
    IntegrationSnapshot Snapshot() const;

private:
    enum class Action { Idle, Connect, Inspect, Save };
    struct Job {
        FileInspectionRequest input;
        std::string sourceId, configId;
        bool commitUnconfirmed = false;
    };
    struct RequestLookup {
        std::string requestId;
        std::future<std::optional<StoredInspection>> result;
    };

    void Run();
    void Inspect();
    void Save();
    void Connect();
    void Queue(Action action, IntegrationState state);
    void Begin(Job job, std::string requestId);
    bool CanStart() const;
    std::string Respond(const device::Request& request, RequestLookup& lookup);

    const std::filesystem::path config_, deviceImage_;
    const std::function<void()> beforeVerdict_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool paused_ = false, closing_ = false, workerClosed_ = false, listenerClosed_ = false;
    std::atomic_bool stopListener_{false}, cancelled_{false};
    Action action_ = Action::Connect;
    std::optional<Job> job_;
    IntegrationSnapshot snapshot_;
    std::vector<unsigned char> deviceConfig_;
    std::thread worker_, listener_;
};
}
