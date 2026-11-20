#include "Protocol.h"

#include <winsock2.h>

#include <array>
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {

using namespace weldscope::device;
using Clock = std::chrono::steady_clock;
constexpr auto frameTimeout = std::chrono::seconds(2);

class Winsock {
public:
    Winsock() {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error != 0) throw std::runtime_error("WSAStartup: " + std::to_string(error));
    }
    ~Winsock() { WSACleanup(); }
    Winsock(const Winsock&) = delete;
    Winsock& operator=(const Winsock&) = delete;
};

class Socket {
public:
    explicit Socket(SOCKET handle) : handle_(handle) {}
    ~Socket() {
        if (handle_ != INVALID_SOCKET) closesocket(handle_);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET Get() const noexcept { return handle_; }

private:
    SOCKET handle_;
};

void RequireSocket(bool succeeded, const char* operation) {
    if (!succeeded) {
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(WSAGetLastError()));
    }
}

void LogSocketError(const char* operation, int error) {
    std::cout << "event=socket_error operation=" << operation << " code=" << error << std::endl;
}

enum class Operation { Receive, Send };

bool WaitReady(SOCKET socket, Operation operation, Clock::time_point deadline) {
    const bool isReceiving = operation == Operation::Receive;
    const char* name = isReceiving ? "receive" : "send";
    const auto remaining = deadline - Clock::now();
    if (remaining > Clock::duration::zero()) {
        const auto microseconds = std::chrono::ceil<std::chrono::microseconds>(remaining).count();
        timeval timeout{
            static_cast<long>(microseconds / 1'000'000),
            static_cast<long>(microseconds % 1'000'000)
        };
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(socket, &ready);
        const int selected = select(0, isReceiving ? &ready : nullptr, isReceiving ? nullptr : &ready, nullptr, &timeout);
        if (selected > 0 && Clock::now() < deadline) return true;
        if (selected == SOCKET_ERROR) {
            LogSocketError(name, WSAGetLastError());
            return false;
        }
    }
    std::cout << "event=timeout operation=" << name << std::endl;
    return false;
}

bool SendFrame(SOCKET socket, std::string_view frame) {
    const auto deadline = Clock::now() + frameTimeout;
    while (!frame.empty()) {
        if (!WaitReady(socket, Operation::Send, deadline)) return false;
        const int sent = send(socket, frame.data(), static_cast<int>(frame.size()), 0);
        if (sent == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) continue;
            LogSocketError("send", error);
            return false;
        }
        if (sent == 0) return false;
        frame.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

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

void ServeConnection(SOCKET socket, Clock::time_point receiveDeadline, SampleHistory& history) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        LogSocketError("nonblocking", WSAGetLastError());
        return;
    }

    FrameReader reader;
    std::string frame;
    std::array<char, 1024> buffer{};
    while (WaitReady(socket, Operation::Receive, receiveDeadline)) {
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) continue;
            LogSocketError("receive", error);
            return;
        }
        if (received == 0) {
            if (reader.HasPartial()) std::cout << "event=incomplete_frame" << std::endl;
            return;
        }

        std::string_view bytes(buffer.data(), static_cast<std::size_t>(received));
        while (!bytes.empty()) {
            const auto status = reader.Consume(bytes, frame);
            if (status == FrameStatus::NeedMore) break;
            const auto request = status == FrameStatus::Complete ? ParseRequest(frame) : std::nullopt;
            if (!request) {
                SendFrame(socket, ErrorFrame("-", "BAD_FRAME"));
                return;
            }
            if (!SendFrame(socket, history.Respond(*request))) return;
            receiveDeadline = Clock::now() + frameTimeout;
        }
    }
}

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

void RunServer(unsigned short port) {
    const Winsock winsock;
    const Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    RequireSocket(listener.Get() != INVALID_SOCKET, "socket");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    RequireSocket(bind(listener.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "bind");
    RequireSocket(listen(listener.Get(), SOMAXCONN) == 0, "listen");
    int addressSize = sizeof(address);
    RequireSocket(getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&address), &addressSize) == 0, "getsockname");
    std::cout << "mode=sample\nport=" << ntohs(address.sin_port) << std::endl;

    SampleHistory history;
    for (;;) {
        const Socket connection(accept(listener.Get(), nullptr, nullptr));
        if (connection.Get() == INVALID_SOCKET) {
            const int error = WSAGetLastError();
            if (error == WSAECONNRESET || error == WSAECONNABORTED) {
                LogSocketError("accept", error);
                continue;
            }
            throw std::runtime_error("accept: " + std::to_string(error));
        }
        ServeConnection(connection.Get(), Clock::now() + frameTimeout, history);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        RunServer(ParsePort(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
