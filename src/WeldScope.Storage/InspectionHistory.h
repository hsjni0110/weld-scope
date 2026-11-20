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
};

struct StoredInspection {
    InspectionRecord record;
    std::string createdAtUtc;
};

struct InspectionSummary {
    std::string inspectionId, sourcePath, decision, createdAtUtc;
    InspectionWorkState workState = InspectionWorkState::Completed;
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
    SaveStatus Save(const InspectionRecord& record) const;
    std::optional<StoredInspection> Get(const std::string& inspectionId) const;
    std::optional<StoredInspection> GetByRequestId(const std::string& requestId) const;
    std::vector<InspectionSummary> Recent() const;

private:
    HistoryConnection settings_;
};

} // namespace weldscope
