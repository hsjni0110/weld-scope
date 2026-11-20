#include "Protocol.h"

#include <algorithm>
#include <array>

namespace weldscope::device {
namespace {

constexpr std::size_t maxFrameBytes = 256;
constexpr std::size_t maxIdBytes = 64;

bool IsAsciiAlphanumeric(char character) {
    return ('a' <= character && character <= 'z') || ('A' <= character && character <= 'Z') ||
           ('0' <= character && character <= '9');
}

bool IsIdentifier(std::string_view field) {
    return !field.empty() && field.size() <= maxIdBytes && IsAsciiAlphanumeric(field.front()) &&
           std::all_of(field.begin() + 1, field.end(), [](char character) {
               return IsAsciiAlphanumeric(character) || character == '_' || character == '-' || character == '.';
           });
}

} // namespace

FrameStatus FrameReader::Consume(std::string_view& bytes, std::string& frame) {
    const auto newline = bytes.find('\n');
    const auto part = bytes.substr(0, newline);
    if (part.size() > maxFrameBytes - pending_.size()) return FrameStatus::TooLong;

    pending_.append(part);
    bytes.remove_prefix(part.size());
    if (newline == std::string_view::npos) return FrameStatus::NeedMore;

    bytes.remove_prefix(1);
    frame.swap(pending_);
    pending_.clear();
    return FrameStatus::Complete;
}

bool FrameReader::HasPartial() const noexcept {
    return !pending_.empty();
}

std::optional<Request> ParseRequest(std::string_view frame) {
    constexpr std::string_view command = "INSPECT\t";
    if (frame.substr(0, command.size()) != command) return std::nullopt;
    frame.remove_prefix(command.size());

    std::array<std::string_view, 3> fields{};
    for (std::size_t index = 0; index + 1 < fields.size(); ++index) {
        const auto separator = frame.find('\t');
        if (separator == std::string_view::npos) return std::nullopt;
        fields[index] = frame.substr(0, separator);
        frame.remove_prefix(separator + 1);
    }
    fields.back() = frame;
    if (!std::all_of(fields.begin(), fields.end(), IsIdentifier)) return std::nullopt;
    return Request{fields[0], fields[1], fields[2]};
}

std::string ResultFrame(std::string_view requestId, std::string_view inspectionId, std::string_view decision) {
    return std::string("RESULT\t").append(requestId).append("\t").append(inspectionId).append("\t").append(decision).append("\n");
}

std::string StatusFrame(std::string_view requestId, std::string_view inspectionId, std::string_view status) {
    return std::string("STATUS\t").append(requestId).append("\t").append(inspectionId).append("\t").append(status).append("\n");
}

std::string ErrorFrame(std::string_view requestId, std::string_view code) {
    return std::string("ERROR\t").append(requestId).append("\t").append(code).append("\n");
}

} // namespace weldscope::device
