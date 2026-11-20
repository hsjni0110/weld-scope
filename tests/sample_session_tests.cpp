#include "SampleSession.h"
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    using namespace weldscope;
    try {
        SampleSession session;
        require(!session.start(), "empty input must not start");
        for (auto verdict : {SampleVerdict::Normal, SampleVerdict::Defect, SampleVerdict::Review}) {
            session.select(verdict, true);
            require(session.verdict() == SampleVerdict::None, "new input clears old result");
            require(session.start(), "loaded sample starts");
            require(!session.start(), "running sample cannot restart");
            require(!session.select(SampleVerdict::Normal, true), "running input is locked");
            require(session.complete(), "running sample completes");
            require(session.verdict() == verdict, "completion reveals fixture verdict");
        }
        session.start();
        require(session.cancel(), "running sample cancels");
        require(!session.complete(), "late completion after cancellation is ignored");
        require(session.verdict() == SampleVerdict::None, "cancel has no verdict");
        session.select(SampleVerdict::Defect, false);
        require(session.state() == SampleState::Error, "decode failure is an error");
        require(!session.start(), "failed image cannot start");
        require(session.verdict() == SampleVerdict::None, "error clears old verdict");
        session.select(SampleVerdict::Normal, true);
        require(session.start() && session.complete(), "valid selection recovers after error");
        std::cout << "PASS: empty input, three verdicts, cancellation, reselection, error recovery\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
