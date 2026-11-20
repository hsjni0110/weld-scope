#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace weldscope::device {

enum class FrameStatus { NeedMore, Complete, TooLong };

class FrameReader {
public:
    FrameStatus Consume(std::string_view& bytes, std::string& frame);
    bool HasPartial() const noexcept;

private:
    std::string pending_;
};

struct Request {
    std::string_view requestId;
    std::string_view sourceId;
    std::string_view configId;
};

// Parsed fields refer to the supplied frame.
std::optional<Request> ParseRequest(std::string_view frame);
std::string ResultFrame(std::string_view requestId, std::string_view inspectionId, std::string_view decision = "FAIL");
std::string StatusFrame(std::string_view requestId, std::string_view inspectionId, std::string_view status);
std::string ErrorFrame(std::string_view requestId, std::string_view code);

} // namespace weldscope::device
