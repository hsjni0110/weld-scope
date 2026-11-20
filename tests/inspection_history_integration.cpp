#include "InspectionHistory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace weldscope;

int main() {
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
        std::cout << "S10 history checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
