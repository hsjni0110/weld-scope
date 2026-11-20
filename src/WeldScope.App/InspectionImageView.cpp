#include "pch.h"
#include "InspectionImageView.h"
#include <algorithm>
#include <utility>

BEGIN_MESSAGE_MAP(InspectionImageView, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

void InspectionImageView::SetImages(cv::Mat original, cv::Mat overlay)
{
    original_ = std::move(original);
    overlay_ = std::move(overlay);
    Invalidate(FALSE);
}

void InspectionImageView::SetCandidateVisible(bool visible)
{
    if (showCandidate_ == visible) return;
    showCandidate_ = visible;
    Invalidate(FALSE);
}

BOOL InspectionImageView::OnEraseBkgnd(CDC*) { return TRUE; }

void InspectionImageView::OnPaint()
{
    CPaintDC dc(this);
    CRect bounds;
    GetClientRect(bounds);
    dc.FillSolidRect(bounds, RGB(19, 28, 41));
    if (original_.empty()) {
        dc.SetBkMode(TRANSPARENT);
        dc.SetTextColor(RGB(196, 206, 220));
        dc.SelectObject(GetParent()->GetFont());
        dc.DrawText(L"표시할 이미지 없음", bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const auto& image = showCandidate_ && HasCandidate() ? overlay_ : original_;
    const int padding = MulDiv(32, GetDpiForWindow(m_hWnd), 96);
    const double scale = (std::min)(static_cast<double>((std::max)(1, bounds.Width() - padding)) / image.cols,
        static_cast<double>((std::max)(1, bounds.Height() - padding)) / image.rows);
    const int width = (std::max)(1, static_cast<int>(image.cols * scale));
    const int height = (std::max)(1, static_cast<int>(image.rows * scale));
    CRect imageRect((bounds.Width() - width) / 2, (bounds.Height() - height) / 2, 0, 0);
    imageRect.right = imageRect.left + width;
    imageRect.bottom = imageRect.top + height;
    dc.SetStretchBltMode(HALFTONE);
    dc.SetBrushOrg(0, 0);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = image.cols;
    info.bmiHeader.biHeight = -image.rows;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    ::StretchDIBits(dc.GetSafeHdc(), imageRect.left, imageRect.top,
        width, height, 0, 0, image.cols, image.rows, image.data, &info, DIB_RGB_COLORS, SRCCOPY);
}
