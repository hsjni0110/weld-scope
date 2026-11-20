#include "Protocol.h"

#include <iostream>
#include <stdexcept>

namespace {

using namespace weldscope::device;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestFrameBoundaries() {
    FrameReader reader;
    std::string frame;
    std::string_view bytes = "INSPECT\treq-";
    Require(reader.Consume(bytes, frame) == FrameStatus::NeedMore && bytes.empty() && reader.HasPartial(),
            "An unfinished frame must be retained without emitting a request");
    bytes = "001\tweld-01\tbaseline-v1\nsecond\nthi";
    Require(reader.Consume(bytes, frame) == FrameStatus::Complete &&
                frame == "INSPECT\treq-001\tweld-01\tbaseline-v1",
            "The next chunk must finish the original frame");
    Require(reader.Consume(bytes, frame) == FrameStatus::Complete && frame == "second",
            "A combined chunk must emit the second frame separately");
    Require(reader.Consume(bytes, frame) == FrameStatus::NeedMore && reader.HasPartial(),
            "The next partial frame must survive after two complete frames");
    bytes = "rd\n";
    Require(reader.Consume(bytes, frame) == FrameStatus::Complete && frame == "third" && !reader.HasPartial(),
            "Finishing a partial frame must clear it");
    bytes = "\n";
    Require(reader.Consume(bytes, frame) == FrameStatus::Complete && frame.empty(),
            "An empty frame must reach the validator");
}

void TestFrameLimit() {
    FrameReader reader;
    std::string frame;
    const std::string body(256, 'A');
    std::string_view bytes = body;
    Require(reader.Consume(bytes, frame) == FrameStatus::NeedMore, "Allow 256 bytes before LF");
    bytes = "\n";
    Require(reader.Consume(bytes, frame) == FrameStatus::Complete && frame == body,
            "Accept LF separately after a 256-byte body");
    bytes = body;
    Require(reader.Consume(bytes, frame) == FrameStatus::NeedMore, "Allow a second 256-byte body");
    bytes = "B";
    Require(reader.Consume(bytes, frame) == FrameStatus::TooLong, "Reject byte 257 without waiting for LF");
}

void TestFields() {
    const auto request = ParseRequest("INSPECT\tReq_1.2-x\tweld-01\tbaseline-v1");
    Require(request && request->requestId == "Req_1.2-x" &&
                request->sourceId == "weld-01" && request->configId == "baseline-v1",
            "Parse the four fields and preserve their spelling");
    for (const std::string_view invalid : {
             "", "GET\tr\ts\tc", "INSPECT\tr\ts", "INSPECT\tr\ts\tc\textra", "INSPECT\t\ts\tc",
             "INSPECT\t-\ts\tc", "INSPECT\tr\ts\tc\r", "INSPECT\tr\ts\tc d", "INSPECT\tr\ts\t\xFF"}) {
        Require(!ParseRequest(invalid), "Reject malformed command, field count, or identifier");
    }
    Require(!ParseRequest(std::string_view("INSPECT\tr\ts\tc\0", 14)), "Reject embedded NUL");
    const std::string maxId(64, 'x');
    Require(ParseRequest("INSPECT\t" + maxId + "\ts\tc").has_value(), "Allow a 64-byte ID");
    Require(!ParseRequest("INSPECT\t" + maxId + "x\ts\tc"), "Reject a 65-byte ID");
    Require(ResultFrame("r", "i-1") == "RESULT\tr\ti-1\tFAIL\n", "Encode the sample response");
    Require(ResultFrame("r", "i-1", "PASS") == "RESULT\tr\ti-1\tPASS\n", "Encode the stored decision");
    Require(StatusFrame("r", "i-1", "IN_PROGRESS") == "STATUS\tr\ti-1\tIN_PROGRESS\n",
            "Encode the status and keep the inspection ID");
    Require(ErrorFrame("-", "BAD_FRAME") == "ERROR\t-\tBAD_FRAME\n", "Encode the error response");
}

} // namespace

int main() {
    try {
        TestFrameBoundaries();
        TestFrameLimit();
        TestFields();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
