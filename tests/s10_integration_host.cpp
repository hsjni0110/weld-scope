#include "IntegratedInspection.h"

#include <chrono>
#include <iomanip>
#include <iostream>

using namespace weldscope;
using namespace std::chrono_literals;

const char* StateName(IntegrationState state) {
    switch (state) {
    case IntegrationState::Connecting: return "CONNECTING";
    case IntegrationState::Disconnected: return "DISCONNECTED";
    case IntegrationState::Ready: return "READY";
    case IntegrationState::Inspecting: return "INSPECTING";
    case IntegrationState::Saving: return "SAVING";
    case IntegrationState::Stored: return "STORED";
    case IntegrationState::Unsaved: return "UNSAVED";
    case IntegrationState::CommitUnknown: return "COMMIT_UNKNOWN";
    case IntegrationState::Conflict: return "CONFLICT";
    }
    throw std::logic_error("Unknown state");
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::mutex verdictMutex;
    std::condition_variable verdictChanged;
    bool holdVerdict = false;
    bool reachedVerdict = false;
    cv::setNumThreads(1);
    IntegratedInspection inspection(argv[1], argv[2], 0, [&] {
        std::unique_lock lock(verdictMutex);
        reachedVerdict = true;
        verdictChanged.notify_all();
        verdictChanged.wait(lock, [&] { return !holdVerdict; });
    }, InspectionMode::Baseline);
    auto snapshot = inspection.Snapshot();
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while ((!snapshot.port || snapshot.state == IntegrationState::Connecting) &&
           !snapshot.listenerFailed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        snapshot = inspection.Snapshot();
    }
    if (!snapshot.port) return 3;
    std::cout << snapshot.port << std::endl;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "quit") break;
        if (command == "inspect" || command == "inspect_hold") {
            {
                const std::lock_guard lock(verdictMutex);
                holdVerdict = command == "inspect_hold";
                reachedVerdict = false;
            }
            const bool accepted = inspection.StartFile(argv[3]);
            std::cout << (accepted ? "ACCEPTED" : "BLOCKED") << std::endl;
        }
        if (command == "wait_verdict") {
            std::unique_lock lock(verdictMutex);
            const bool reached = verdictChanged.wait_for(lock, 5s, [&] { return reachedVerdict; });
            std::cout << (reached ? "AT_VERDICT" : "TIMEOUT") << std::endl;
        }
        if (command == "release_verdict") {
            const std::lock_guard lock(verdictMutex);
            holdVerdict = false;
            verdictChanged.notify_all();
        }
        if (command == "retry") inspection.Retry();
        if (command == "cancel") inspection.CancelInspection();
        if (command == "save_ms") {
            const auto value = inspection.Snapshot().saveMs;
            std::cout << std::setprecision(17) << value.value_or(-1.0) << std::endl;
        }
        if (command == "state") {
            snapshot = inspection.Snapshot();
            std::cout << StateName(snapshot.state) << ' ' << snapshot.record.inspectionId << ' '
                << snapshot.record.decision << ' ' << snapshot.record.attempts.size() << std::endl;
        }
    }
    const std::lock_guard lock(verdictMutex);
    holdVerdict = false;
    verdictChanged.notify_all();
}
