#pragma once

#include <opencv2/core.hpp>
class InspectionImageView : public CWnd {
public:
    void SetImages(cv::Mat original, cv::Mat overlay);
    void SetCandidateVisible(bool visible);
    bool HasCandidate() const { return !overlay_.empty(); }
protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    DECLARE_MESSAGE_MAP()
private:
    cv::Mat original_, overlay_;
    bool showCandidate_ = false;
};
