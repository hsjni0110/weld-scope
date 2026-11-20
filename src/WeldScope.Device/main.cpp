#include "LoopbackServer.h"

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {
using namespace weldscope::device;

class SampleHistory {
public:
    std::string Respond(const Request& request) {
        std::string key(request.requestId);
        const auto existing = replies_.find(key);
        if (existing != replies_.end()) {
            const auto& reply = existing->second;
            if (reply.sourceId != request.sourceId || reply.configId != request.configId) {
                return ErrorFrame(request.requestId, "REQUEST_CONFLICT");
            }
            LogResult("replayed", request.requestId, reply.inspectionId);
            return reply.frame;
        }
        if (request.sourceId != "weld-01") return ErrorFrame(request.requestId, "UNKNOWN_SOURCE");
        if (request.configId != "baseline-v1") return ErrorFrame(request.requestId, "UNKNOWN_CONFIG");
        if (replies_.size() >= maxRequests) return ErrorFrame(request.requestId, "CAPACITY");

        const auto inspectionId = "i-" + std::to_string(replies_.size() + 1);
        const auto [entry, inserted] = replies_.emplace(
            std::move(key),
            Reply{std::string(request.sourceId), std::string(request.configId), inspectionId,
                  ResultFrame(request.requestId, inspectionId)});
        LogResult("created", request.requestId, inspectionId);
        return entry->second.frame;
    }

private:
    struct Reply {
        std::string sourceId;
        std::string configId;
        std::string inspectionId;
        std::string frame;
    };

    static void LogResult(const char* event, std::string_view requestId, std::string_view inspectionId) {
        std::cout << "event=" << event << " request_id=" << requestId
                  << " inspection_id=" << inspectionId << std::endl;
    }

    // ponytail: process-local sample history, persistent replay belongs to S10.
    static constexpr std::size_t maxRequests = 256;
    std::unordered_map<std::string, Reply> replies_;
};

unsigned short ParsePort(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--port") {
        throw std::invalid_argument("Usage: weldscope_device --port <0-65535>");
    }
    const std::string_view argument(argv[2]);
    unsigned short port = 0;
    const auto [end, error] = std::from_chars(argument.data(), argument.data() + argument.size(), port);
    if (error != std::errc{} || end != argument.data() + argument.size()) {
        throw std::invalid_argument("Invalid port. Expected an integer from 0 to 65535.");
    }
    return port;
}

} // namespace

int main(int argc, char** argv) {
    try {
        SampleHistory history;
        const std::atomic_bool stop(false);
        ServeLoopback(ParsePort(argc, argv), stop,
            [&history](const Request& request) { return history.Respond(request); },
            [](unsigned short port) { std::cout << "mode=sample\nport=" << port << std::endl; });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
