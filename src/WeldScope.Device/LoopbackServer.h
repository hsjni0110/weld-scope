#pragma once

#include "Protocol.h"

#include <atomic>
#include <functional>

namespace weldscope::device {
using RequestHandler = std::function<std::string(const Request&)>;
using ListeningHandler = std::function<void(unsigned short)>;
void ServeLoopback(unsigned short port, const std::atomic_bool& stop,
                   const RequestHandler& respond, const ListeningHandler& listening);
}
