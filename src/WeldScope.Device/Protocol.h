#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace weldscope::device {

// 줄 하나가 끝났는지, 더 받을 바이트가 있는지, 본문 한도를 넘었는지 알려 준다.
enum class FrameStatus { NeedMore, Complete, TooLong };

// 한 TCP 연결의 수신 조각을 모아 LF로 끝나는 요청 한 줄을 꺼낸다.
class FrameReader {
public:
    // 읽은 만큼 bytes를 줄인다. Complete면 LF를 제외한 본문을 frame에 쓴다.
    // TooLong이면 해당 연결의 프레임 처리를 중단한다.
    FrameStatus Consume(std::string_view& bytes, std::string& frame);
    // 연결 종료나 시간 초과 시 아직 끝나지 않은 요청이 있었는지 확인한다.
    bool HasPartial() const noexcept;

private:
    std::string pending_;
};

// 프레임 버퍼의 필드를 빌려 쓴다. 원본을 교체하거나 해제하면 사용할 수 없다.
struct Request {
    std::string_view requestId;
    std::string_view sourceId;
    std::string_view configId;
};

// LF를 뺀 INSPECT 본문에서 요청, 입력, 설정 ID를 읽는다. 문법이 맞지 않으면 std::nullopt를 반환한다.
std::optional<Request> ParseRequest(std::string_view frame);
// 저장이 확인된 검사의 판정을 장비에 보낼 RESULT 한 줄로 만든다.
std::string ResultFrame(std::string_view requestId, std::string_view inspectionId, std::string_view decision = "FAIL");
// 처리 중, 취소, 저장 대기처럼 검사 판정과 구별되는 상태를 STATUS 한 줄로 만든다.
std::string StatusFrame(std::string_view requestId, std::string_view inspectionId, std::string_view status);
// 요청을 처리하지 못한 원인을 ERROR 한 줄로 만든다. 검사 ID가 없는 경우에도 보낼 수 있다.
std::string ErrorFrame(std::string_view requestId, std::string_view code);

} // namespace weldscope::device
