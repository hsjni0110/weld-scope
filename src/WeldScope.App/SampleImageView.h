#pragma once
#include <atlimage.h>

class SampleImageView : public CWnd {
public:
    bool LoadResourceImage(UINT resourceId);
    void SetCandidateVisible(bool visible);
    CSize ImageSize() const;
protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    DECLARE_MESSAGE_MAP()
private:
    CImage image_;
    bool hasCandidate_ = false;
};
