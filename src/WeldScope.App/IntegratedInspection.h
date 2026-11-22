#pragma once

#include "FileInspectionJob.h"
#include "InspectionHistory.h"
#include "Protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace weldscope {
// 검사와 MySQL 저장을 함께 제어할 때 쓰는 상태. CommitUnknown은 커밋 여부를 확인하지 못했다는 뜻이다.
enum class IntegrationState { Connecting, Disconnected, Ready, Inspecting, Saving, Stored, Unsaved, CommitUnknown, Conflict };

// 하위 폴더까지 검색해 PNG의 상대 경로를 정렬해 반환한다. 재분석 지점은 건너뛰고 검색 오류는 예외로 알린다.
std::vector<std::filesystem::path> FindInspectionFiles(const std::filesystem::path& root);

// 폴더 검사 진행 상황. savedFiles는 저장을 확인한 파일 수이며 취소 기록도 포함한다.
struct BatchProgress {
    std::string batchId;
    unsigned int totalFiles = 0, savedFiles = 0;
    bool active = false;
    bool stopping = false;
};

// 화면이 조정기의 내부 상태를 직접 참조하지 않고 읽을 수 있는 복사본.
// port가 0이면 수신 포트가 준비되지 않았다. modelReady가 false이면 파일 검사를 시작할 수 없다.
struct IntegrationSnapshot {
    IntegrationState state = IntegrationState::Connecting;
    unsigned short port = 0;
    bool listenerFailed = false;
    bool modelReady = false;
    std::optional<double> saveMs;
    std::uint64_t revision = 0;
    InspectionRecord record;
    std::optional<FileInspectionResult> result;
    std::optional<BatchProgress> batch;
    bool batchReady = false;
};

// 파일, 폴더, TCP 요청을 한 번에 하나의 검사 및 저장 작업으로 조정한다.
class IntegratedInspection {
public:
    // 워커와 루프백 리스너를 시작한다. 포트 0이면 운영체제가 선택하고 Snapshot에서 확인할 수 있다.
    // beforeVerdict는 분석 호출이 반환된 뒤 워커에서 호출하는 검수용 콜백이다.
    IntegratedInspection(std::filesystem::path config, std::filesystem::path deviceImage, unsigned short port,
                         std::function<void()> beforeVerdict = {});
    // 종료를 요청한 뒤 워커와 리스너가 끝날 때까지 기다린다.
    ~IntegratedInspection();
    IntegratedInspection(const IntegratedInspection&) = delete;
    IntegratedInspection& operator=(const IntegratedInspection&) = delete;

    // 단일 파일 검사를 요청한다. true는 요청 수락을 뜻하며 검사 완료와는 별개다.
    bool TryStartFile(const std::filesystem::path& path);
    // 절대 루트와 정렬된 상대 경로 목록을 받는다. 빈 목록, 중복, 상위 경로 이동은 거부한다.
    // 요청을 받으면 폴더 이력을 먼저 만들고, 파일을 한 장씩 검사해 저장한다.
    bool TryStartFolder(const std::filesystem::path& root, std::vector<std::filesystem::path> files);
    // 판정 전이면 취소를 요청한다. 폴더 작업은 다음 입력을 시작하지 않도록 표시한다.
    void CancelInspection();
    // 연결이 끊겼으면 다시 연결한다. 저장 대기 중이면 같은 작업을 분석 없이 재저장한다.
    void Retry();
    // 새 검사 수락을 일시 중지하거나 재개한다. 이미 시작한 작업은 계속 진행한다.
    void Pause(bool paused);
    // 새 요청을 막고 워커와 리스너에 종료를 요청한다. 종료를 기다리지는 않는다.
    void Close();
    // 워커와 리스너가 모두 종료됐을 때 true를 반환한다.
    bool Closed() const;
    // 여러 필드가 같은 시점의 값이도록 잠금 아래 상태를 복사한다.
    IntegrationSnapshot Snapshot() const;

private:
    enum class Action { Idle, Connect, CreateBatch, Inspect, Save, FinishBatch };
    struct Job {
        FileInspectionRequest input;
        std::string sourceId, configId;
        bool commitUnconfirmed = false;
    };
    struct FolderWork {
        std::filesystem::path root;
        std::vector<std::filesystem::path> files;
        std::shared_ptr<const PooledSmoothModel> model;
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
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool paused_ = false, closing_ = false, workerClosed_ = false, listenerClosed_ = false;
    std::atomic_bool stopListener_{false}, cancelled_{false};
    Action action_ = Action::Connect;
    std::optional<Job> job_;
    std::optional<FolderWork> folder_;
    std::optional<Action> folderRetry_;
    IntegrationSnapshot snapshot_;
    std::shared_ptr<const PooledSmoothModel> model_;
    std::thread worker_, listener_;
};
}
