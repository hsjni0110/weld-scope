#pragma once

#include <afxwin.h>
#include <opencv2/core.hpp>
// 검사 화면의 UI 스레드에서 원본 또는 후보가 합성된 영상을 그린다.
class InspectionImageView : public CWnd {
public:
    // 연속 메모리인 32비트 BGRA 영상 두 장을 보관한다. 빈 원본은 표시할 영상이 없다는 뜻이다.
    // cv::Mat이 공유하는 픽셀은 표시 중에 변경하지 않는다.
    void SetImage(cv::Mat original);
    // 후보 영상 표시를 요청한다. 후보가 없으면 원본을 그린다.
    // 표시 여부와 무관하게 합성된 후보 영상이 준비됐는지 알려 준다.
protected:
    // Windows가 다시 그리기를 요청하면 영상 비율을 유지해 영역 안에 배치한다.
    afx_msg void OnPaint();
    // 그리기에서 배경을 함께 채우므로 별도 배경 지우기를 생략한다.
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    DECLARE_MESSAGE_MAP()
private:
    cv::Mat original_;
};
