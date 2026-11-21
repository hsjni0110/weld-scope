#pragma once

#include "Protocol.h"

#include <atomic>
#include <functional>

namespace weldscope::device {
// 수신 스레드에서 요청을 받아 전송할 응답 한 줄을 반환한다. Request는 호출 중에만 유효하다.
using RequestHandler = std::function<std::string(const Request&)>;
// 리스너가 준비되면 운영체제가 실제로 연 포트를 한 번 전달한다.
using ListeningHandler = std::function<void(unsigned short)>;
// 127.0.0.1에서 연결을 순차적으로 처리한다. 포트 0은 운영체제가 포트를 선택하게 한다.
// stop을 요청할 때까지 호출 스레드를 사용한다. 리스너 초기화 실패는 예외로 알린다.
void ServeLoopback(unsigned short port, const std::atomic_bool& stop,
                   const RequestHandler& respond, const ListeningHandler& listening);
}
