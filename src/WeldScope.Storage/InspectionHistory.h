#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace weldscope {

// 저장소가 연결마다 사용할 접속 설정. caFile을 지정하면 서버 인증서와 호스트 이름을 확인한다.
struct HistoryConnection {
    std::string host;
    unsigned int port;
    std::string database;
    std::string user;
    std::string password;
    std::string caFile;
};
// WELDSCOPE_MYSQL_* 환경 변수에서 설정을 읽는다. 암호가 없거나 포트가 잘못되면 예외를 던진다.
HistoryConnection HistoryConnectionFromEnvironment();

// 실행한 분석 한 단계의 기록. 점수는 비어 있을 수 있고 durationMs는 밀리초다.
struct AnalysisAttempt {
    std::string stage;
    std::optional<double> score;
    std::string decision;
    std::string reason;
    double durationMs;

    // 같은 저장 요청인지 비교하기 위해 시간과 점수까지 확인한다.
    bool operator==(const AnalysisAttempt& other) const;
};

// 분석이 종료된 경우와 사용자가 판정 전에 취소한 경우를 구분한다.
enum class InspectionWorkState { Completed, Cancelled };

// 저장과 재시도에 사용하는 검사 기록. 재시도할 때 두 ID와 내용을 그대로 유지한다.
struct InspectionRecord {
    std::string inspectionId;
    std::string requestId;
    std::string inputId;
    std::string sourcePath;
    std::string sourceSha256;
    std::string configSha256;
    std::string algorithmVersion;
    std::string datasetVersion;
    // 취소된 작업에는 판정과 분석 시도가 없다.
    std::string decision;
    std::string reason;
    std::vector<AnalysisAttempt> attempts;
    InspectionWorkState workState = InspectionWorkState::Completed;
    // 폴더 검사에서만 묶음 ID와 1부터 시작하는 파일 순서를 지정한다.
    std::string batchId = {};
    unsigned int batchIndex = 0;
};

// 상세 조회에 사용하는 저장 기록과 UTC 저장 시각.
struct StoredInspection {
    InspectionRecord record;
    std::string createdAtUtc;
};

// 검사 목록을 표시하는 데 필요한 값만 담는다.
struct InspectionSummary {
    std::string inspectionId, sourcePath, decision, createdAtUtc;
    InspectionWorkState workState = InspectionWorkState::Completed;
};

// 폴더 검사 목록의 요약. totalFiles는 저장 건수가 아니라 시작 시 확정한 입력 수다.
struct BatchSummary {
    std::string batchId, rootPath, state;
    unsigned int totalFiles = 0;
};

// 한 폴더 검사에서 저장을 확인한 건수를 판정과 취소로 나눈다.
struct BatchCounts {
    unsigned int passed = 0, failed = 0, review = 0, errors = 0, cancelled = 0;

    // 아직 저장하지 못한 입력은 제외하며, 저장된 취소 건은 포함한다.
    unsigned int Total() const { return passed + failed + review + errors + cancelled; }
};

// 폴더 검사 목록의 한 행. durationMs는 저장된 분석 시도 시간을 합산한 값이다.
struct BatchRow {
    std::string inspectionId, sourcePath, decision, reason;
    double durationMs = 0;
    InspectionWorkState workState = InspectionWorkState::Completed;
};

// 같은 DB 읽기 시점의 전체 집계와 현재 필터에 맞는 결과 페이지.
struct BatchPageView {
    BatchCounts counts;
    std::vector<BatchRow> rows;
};

// 신규 저장(Stored), 기존 내용 일치(Replay), 기존 번호나 순서에 다른 기록 존재(Conflict).
enum class SaveStatus { Stored, Replay, Conflict };

// DB의 커밋 여부를 확인하지 못했다. 같은 ID와 내용으로 다시 저장해 확인할 수 있다.
class CommitOutcomeUnknown : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// MySQL에서 검사 결과와 폴더 검사 이력을 읽고 쓴다. 호출이 끝날 때까지 DB 작업을 기다린다.
class InspectionHistory {
public:
    // 접속 설정만 보관한다. 연결은 개별 작업에서 연다.
    explicit InspectionHistory(HistoryConnection settings);

    // 이 설정으로 연결한 MySQL 서버가 보고하는 버전을 읽는다.
    std::string Version() const;
    // 단일 검사 저장과 조회에 필요한 테이블에 접근할 수 있는지 확인한다. 실패하면 예외를 던진다.
    void CheckReady() const;
    // 폴더 검사에 필요한 테이블과 쓰기 권한을 확인한다. CheckReady와 별도로 호출한다.
    void CheckBatchReady() const;
    // 검사, 분석 시도, 폴더 소속을 한 트랜잭션으로 보존한다.
    // CommitOutcomeUnknown이면 같은 ID와 내용으로 다시 호출한다.
    SaveStatus Save(const InspectionRecord& record) const;
    // 검사 ID로 상세 기록을 읽는다. 해당 기록이 없으면 std::nullopt를 반환한다.
    std::optional<StoredInspection> Get(const std::string& inspectionId) const;
    // 장비가 재전송한 요청 ID로 검사 기록을 찾는다. 해당 기록이 없으면 std::nullopt를 반환한다.
    std::optional<StoredInspection> GetByRequestId(const std::string& requestId) const;
    // 최신 검사부터 최대 50건을 반환한다.
    std::vector<InspectionSummary> Recent() const;
    // 새 묶음을 running 상태로 연다. 동일한 running 묶음의 재호출은 허용하고 충돌은 예외로 알린다.
    void CreateBatch(const std::string& batchId, const std::string& rootPath,
                     const std::string& configSha256, unsigned int totalFiles) const;
    // completed 또는 stopped로 닫는다. completed는 모든 입력이 저장됐을 때만 허용한다.
    // 이미 같은 상태로 닫은 묶음은 다시 요청할 수 있다.
    void FinishBatch(const std::string& id, const std::string& state) const;
    // 최신 폴더 검사부터 최대 50건을 반환한다.
    std::vector<BatchSummary> RecentBatches() const;
    // 전체 집계와 최대 100행을 한 시점에서 읽는다. 빈 필터는 전체이며 offset은 필터 적용 이후의 행 번호다.
    BatchPageView ReadBatchPage(const std::string& id, const std::string& filter, unsigned int offset) const;

private:
    HistoryConnection settings_;
};

} // namespace weldscope
