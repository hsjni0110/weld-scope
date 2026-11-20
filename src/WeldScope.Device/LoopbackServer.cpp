#include "LoopbackServer.h"

#include <winsock2.h>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace weldscope::device {
namespace {

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

void ServeConnection(SOCKET socket, Clock::time_point receiveDeadline, const RequestHandler& respond, const std::atomic_bool& stop) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        LogSocketError("nonblocking", WSAGetLastError());
        return;
    }

    FrameReader reader;
    std::string frame;
    std::array<char, 1024> buffer{};
    while (!stop && WaitReady(socket, Operation::Receive, receiveDeadline)) {
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
        while (!stop && !bytes.empty()) {
            const auto status = reader.Consume(bytes, frame);
            if (status == FrameStatus::NeedMore) break;
            const auto request = status == FrameStatus::Complete ? ParseRequest(frame) : std::nullopt;
            if (!request) {
                SendFrame(socket, ErrorFrame("-", "BAD_FRAME"));
                return;
            }
            if (!SendFrame(socket, respond(*request))) return;
            receiveDeadline = Clock::now() + frameTimeout;
        }
    }
}

} // namespace

void ServeLoopback(unsigned short port, const std::atomic_bool& stop,
                   const RequestHandler& respond, const ListeningHandler& listening) {
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
    listening(ntohs(address.sin_port));

    while (!stop) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener.Get(), &ready);
        timeval timeout{0, 100000};
        const int selected = select(0, &ready, nullptr, nullptr, &timeout);
        RequireSocket(selected != SOCKET_ERROR, "select listener");
        if (selected == 0 || stop) continue;
        const Socket connection(accept(listener.Get(), nullptr, nullptr));
        if (connection.Get() == INVALID_SOCKET) {
            const int error = WSAGetLastError();
            if (error == WSAECONNRESET || error == WSAECONNABORTED) {
                LogSocketError("accept", error);
                continue;
            }
            throw std::runtime_error("accept: " + std::to_string(error));
        }
        ServeConnection(connection.Get(), Clock::now() + frameTimeout, respond, stop);
    }
}

} // namespace weldscope::device
