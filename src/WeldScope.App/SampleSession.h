#pragma once

namespace weldscope {
enum class SampleVerdict { None, Normal, Defect, Review };
enum class SampleState { Empty, Ready, Running, Complete, Cancelled, Error };

// A presentation fixture session, independent of any inspection engine.
class SampleSession {
public:
    bool select(SampleVerdict expectedVerdict, bool imageLoaded)
    {
        if (state_ == SampleState::Running) return false;
        expectedVerdict_ = expectedVerdict;
        verdict_ = SampleVerdict::None;
        state_ = imageLoaded ? SampleState::Ready : SampleState::Error;
        return true;
    }
    bool start()
    {
        if (state_ != SampleState::Ready && state_ != SampleState::Complete &&
            state_ != SampleState::Cancelled) return false;
        verdict_ = SampleVerdict::None;
        state_ = SampleState::Running;
        return true;
    }
    bool complete()
    {
        if (state_ != SampleState::Running) return false;
        verdict_ = expectedVerdict_;
        state_ = SampleState::Complete;
        return true;
    }
    bool cancel()
    {
        if (state_ != SampleState::Running) return false;
        state_ = SampleState::Cancelled;
        verdict_ = SampleVerdict::None;
        return true;
    }
    SampleState state() const { return state_; }
    SampleVerdict verdict() const { return verdict_; }
private:
    SampleState state_ = SampleState::Empty;
    SampleVerdict verdict_ = SampleVerdict::None;
    SampleVerdict expectedVerdict_ = SampleVerdict::None;
};
}
