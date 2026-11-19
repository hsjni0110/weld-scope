#include "InspectionHistory.h"

#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

using namespace weldscope;

std::string Environment(const char* name, const char* fallback = "") {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

HistoryConnection ConnectionSettings() {
    const auto portText = Environment("WELDSCOPE_MYSQL_PORT", "3309");
    unsigned int port = 0;
    const auto end = portText.data() + portText.size();
    const auto parsed = std::from_chars(portText.data(), end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port == 0 || port > 65535) {
        throw std::invalid_argument("Invalid WELDSCOPE_MYSQL_PORT");
    }
    auto password = Environment("WELDSCOPE_MYSQL_PASSWORD");
    if (password.empty()) throw std::invalid_argument("WELDSCOPE_MYSQL_PASSWORD is required");
    return {Environment("WELDSCOPE_MYSQL_HOST", "10.0.2.2"), port,
            Environment("WELDSCOPE_MYSQL_DATABASE", "weldscope_s05"),
            Environment("WELDSCOPE_MYSQL_USER", "weldscope_app"), std::move(password),
            Environment("WELDSCOPE_MYSQL_CA_FILE")};
}

InspectionRecord Sample(const char* inspectionId, const char* requestId, std::string_view variant) {
    // Fixed history sample based on the first public OPENZDM S04 sample row.
    InspectionRecord record{
        inspectionId, requestId, "openzdm:train/KO/image_1.png", "train/KO/image_1.png",
        "4a08fa2c6274e603f18358134092e8f697cc81f78a36f472577bca81bc0de3f3",
        "266b8fbb2b4463ba2423126bc9619d45831e7e151862f73f7aaaa88f892bedb2",
        "global-dark-ratio-v1", "openzdm-14025630", "FAIL", "dark_pixel_ratio",
        {{"BASELINE", 0.96902173913043477, "FAIL", "dark_pixel_ratio", 0.048917}}};
    if (variant == "changed-score") record.attempts.front().score = 1.0;
    if (variant == "oversized-input") record.inputId = std::string(255, 'x') + ' ';
    if (variant == "oversized-stage") record.attempts.front().stage = std::string(32, 'x') + ' ';
    if (variant == "invalid-attempt" || variant == "two-attempts") {
        record.attempts.push_back({"PRECISE", std::nullopt, "ERROR", "score_unavailable",
                                   variant == "invalid-attempt" ? -1.0 : 1.25});
    }
    return record;
}

void PrintInspection(const StoredInspection& stored) {
    const auto& record = stored.record;
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10)
              << "status=found\ninspection_id=" << record.inspectionId << "\nrequest_id=" << record.requestId
              << "\ninput_id=" << record.inputId << "\nsource_path=" << record.sourcePath
              << "\nsource_sha256=" << record.sourceSha256 << "\nconfig_sha256=" << record.configSha256
              << "\nalgorithm_version=" << record.algorithmVersion << "\ndataset_version=" << record.datasetVersion
              << "\ndecision=" << record.decision << "\nreason=" << record.reason
              << "\ncreated_at_utc=" << stored.createdAtUtc << "\nattempt_count=" << record.attempts.size() << '\n';
    std::size_t sequence = 0;
    for (const auto& attempt : record.attempts) {
        const auto prefix = "attempt." + std::to_string(++sequence) + '.';
        std::cout << prefix << "stage=" << attempt.stage << '\n' << prefix << "score=";
        if (attempt.score) std::cout << *attempt.score;
        else std::cout << "NULL";
        std::cout << '\n' << prefix << "decision=" << attempt.decision << '\n' << prefix << "reason=" << attempt.reason
                  << '\n' << prefix << "duration_ms=" << attempt.durationMs << '\n';
    }
}

void PrintSaveStatus(std::string_view status, const InspectionRecord& record) {
    std::cout << "status=" << status << "\ninspection_id=" << record.inspectionId
              << "\nrequest_id=" << record.requestId << "\ndecision=" << record.decision << '\n';
}

int Save(const InspectionRecord& record) {
    try {
        switch (InspectionHistory(ConnectionSettings()).Save(record)) {
        case SaveStatus::Stored: PrintSaveStatus("stored", record); return 0;
        case SaveStatus::Replay: PrintSaveStatus("replay", record); return 0;
        case SaveStatus::Conflict: PrintSaveStatus("conflict", record); return 3;
        }
    } catch (const CommitOutcomeUnknown& error) {
        PrintSaveStatus("commit_unknown", record);
        std::cerr << error.what() << '\n';
        return 5;
    } catch (const std::exception& error) {
        PrintSaveStatus("failed", record);
        std::cerr << error.what() << '\n';
        return 1;
    }
    throw std::logic_error("Invalid save status");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view command = argc > 1 ? argv[1] : "";
        if (command == "version" && argc == 2) {
            const auto version = InspectionHistory(ConnectionSettings()).Version();
            std::cout << "server_version=" << version << '\n';
            return 0;
        }
        if (command == "get" && argc == 3) {
            const auto stored = InspectionHistory(ConnectionSettings()).Get(argv[2]);
            if (stored) {
                PrintInspection(*stored);
                return 0;
            }
            std::cout << "status=not_found\n";
            return 4;
        }
        if (command == "save" && (argc == 4 || argc == 5)) {
            const std::string_view variant = argc == 5 ? argv[4] : "";
            if (variant.empty() || variant == "changed-score" || variant == "invalid-attempt" ||
                variant == "two-attempts" || variant == "oversized-input" || variant == "oversized-stage") {
                return Save(Sample(argv[2], argv[3], variant));
            }
        }
        std::cerr << "Usage: weldscope_history version | get INSPECTION_ID | save INSPECTION_ID REQUEST_ID "
                     "[changed-score|invalid-attempt|two-attempts|oversized-input|oversized-stage]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cout << "status=failed\n";
        std::cerr << error.what() << '\n';
        return 1;
    }
}
