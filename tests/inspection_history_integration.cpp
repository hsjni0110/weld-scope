#include "InspectionHistory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace weldscope;

namespace {

void CheckBatch(const InspectionHistory& history, const std::string& suffix, const std::string& hash) {
    history.CheckBatchReady();
    const auto batchId = "s13-test-batch-" + suffix;
    history.CreateBatch(batchId, "fixture", hash, 2);
    history.CreateBatch(batchId, "fixture", hash, 2);

    InspectionRecord row;
    row.inspectionId = "s13-test-inspection-" + suffix;
    row.requestId = "s13-test-request-" + suffix;
    row.inputId = "fixture";
    row.sourcePath = "fixture/png";
    row.sourceSha256 = hash;
    row.configSha256 = hash;
    row.algorithmVersion = "fixture-v1";
    row.datasetVersion = "fixture";
    row.decision = "PASS";
    row.reason = "fast_threshold";
    row.attempts.push_back({"FAST", 0.1, "PASS", "fast_threshold", 0.125});
    row.batchId = batchId;
    row.batchIndex = 1;
    if (history.Save(row) != SaveStatus::Stored || history.Save(row) != SaveStatus::Replay)
        throw std::runtime_error("Batch member was not stored or replayed");
    auto mismatched = row;
    mismatched.batchIndex = 2;
    if (history.Save(mismatched) != SaveStatus::Conflict)
        throw std::runtime_error("Changed membership did not conflict");
    auto occupied = row;
    occupied.inspectionId += "-occupied";
    occupied.requestId += "-occupied";
    if (history.Save(occupied) != SaveStatus::Conflict || history.Get(occupied.inspectionId))
        throw std::runtime_error("Sequence collision left an inspection without membership");

    history.FinishBatch(batchId, "stopped");
    history.FinishBatch(batchId, "stopped");
    const auto [counts, page] = history.ReadBatchPage(batchId, "PASS", 0);
    if (counts.passed != 1 || counts.failed || counts.review || counts.errors || counts.cancelled)
        throw std::runtime_error("Batch counts include an unstored member");
    if (page.size() != 1 || page.front().inspectionId != row.inspectionId ||
        page.front().reason != row.reason || page.front().durationMs != 0.125 ||
        !history.ReadBatchPage(batchId, "PASS", 1).rows.empty() ||
        !history.ReadBatchPage(batchId, "FAIL", 0).rows.empty())
        throw std::runtime_error("Batch page did not respect filter or offset");
    const auto recent = history.RecentBatches();
    if (recent.empty() || recent.front().batchId != batchId || recent.front().rootPath != "fixture" ||
        recent.front().state != "stopped" || recent.front().totalFiles != 2)
        throw std::runtime_error("Stopped batch was not found in recent batch history");
}

}

int main(int argc, char** argv) {
    try {
        InspectionHistory history(HistoryConnectionFromEnvironment());
        history.CheckReady();
        const std::string hash(64, 'a');
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        InspectionRecord cancelled{"s10-test-inspection-" + suffix, "s10-test-request-" + suffix, "weld-01", "fixture.png",
            "", hash, "global-dark-ratio-v1", "openzdm-14025630", "", "cancelled", {},
            InspectionWorkState::Cancelled};
        const auto saved = history.Save(cancelled);
        if (saved != SaveStatus::Stored && saved != SaveStatus::Replay)
            throw std::runtime_error("Cancelled inspection was not saved");
        const auto found = history.GetByRequestId(cancelled.requestId);
        if (!found || found->record.inspectionId != cancelled.inspectionId ||
            found->record.workState != InspectionWorkState::Cancelled || !found->record.decision.empty() ||
            !found->record.sourceSha256.empty() || !found->record.attempts.empty())
            throw std::runtime_error("Cancelled inspection was not retrieved");
        if (history.Save(cancelled) != SaveStatus::Replay)
            throw std::runtime_error("Saved cancellation did not replay");
        const auto recent = history.Recent();
        const auto summary = std::find_if(recent.begin(), recent.end(), [&](const InspectionSummary& value) {
            return value.inspectionId == cancelled.inspectionId;
        });
        if (recent.size() > 50 || summary == recent.end() || summary->workState != InspectionWorkState::Cancelled ||
            !summary->decision.empty() || summary->sourcePath != cancelled.sourcePath ||
            summary->createdAtUtc != found->createdAtUtc)
            throw std::runtime_error("Recent history was missing or exceeded its limit");
        const auto mode = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
        if (mode == "--batch") CheckBatch(history, suffix, hash);
        if (mode == "--no-batch") {
            bool unavailable = false;
            try { history.CheckBatchReady(); }
            catch (const std::runtime_error&) { unavailable = true; }
            if (!unavailable) throw std::runtime_error("Unavailable batch schema was accepted");
        }
        std::cout << "S10 history checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
