#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace weldscope {

struct HistoryConnection {
    std::string host;
    unsigned int port;
    std::string database;
    std::string user;
    std::string password;
    std::string caFile;
};
HistoryConnection HistoryConnectionFromEnvironment();

struct AnalysisAttempt {
    std::string stage;
    std::optional<double> score;
    std::string decision;
    std::string reason;
    double durationMs;

    bool operator==(const AnalysisAttempt& other) const;
};

enum class InspectionWorkState { Completed, Cancelled };

struct InspectionRecord {
    std::string inspectionId;
    std::string requestId;
    std::string inputId;
    std::string sourcePath;
    std::string sourceSha256;
    std::string configSha256;
    std::string algorithmVersion;
    std::string datasetVersion;
    std::string decision;
    std::string reason;
    std::vector<AnalysisAttempt> attempts;
    InspectionWorkState workState = InspectionWorkState::Completed;
    std::string batchId = {};
    unsigned int batchIndex = 0;
};

struct StoredInspection {
    InspectionRecord record;
    std::string createdAtUtc;
};

struct InspectionSummary {
    std::string inspectionId, sourcePath, decision, createdAtUtc;
    InspectionWorkState workState = InspectionWorkState::Completed;
};

struct BatchSummary {
    std::string batchId, rootPath, state;
    unsigned int totalFiles = 0;
};

struct BatchCounts {
    unsigned int passed = 0, failed = 0, review = 0, errors = 0, cancelled = 0;

    unsigned int Total() const { return passed + failed + review + errors + cancelled; }
};

struct BatchRow {
    std::string inspectionId, sourcePath, decision, reason;
    double durationMs = 0;
    InspectionWorkState workState = InspectionWorkState::Completed;
};

struct BatchPageView {
    BatchCounts counts;
    std::vector<BatchRow> rows;
};

enum class SaveStatus { Stored, Replay, Conflict };

class CommitOutcomeUnknown : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class InspectionHistory {
public:
    explicit InspectionHistory(HistoryConnection settings);

    std::string Version() const;
    void CheckReady() const;
    void CheckBatchReady() const;
    SaveStatus Save(const InspectionRecord& record) const;
    std::optional<StoredInspection> Get(const std::string& inspectionId) const;
    std::optional<StoredInspection> GetByRequestId(const std::string& requestId) const;
    std::vector<InspectionSummary> Recent() const;
    void CreateBatch(const std::string& batchId, const std::string& rootPath,
                     const std::string& configSha256, unsigned int totalFiles) const;
    void FinishBatch(const std::string& id, const std::string& state) const;
    std::vector<BatchSummary> RecentBatches() const;
    BatchPageView ReadBatchPage(const std::string& id, const std::string& filter, unsigned int offset) const;

private:
    HistoryConnection settings_;
};

} // namespace weldscope
