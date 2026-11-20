#include "pch.h"
#include "SampleImageView.h"
#include <shlwapi.h>
#include <algorithm>
#pragma comment(lib, "shlwapi.lib")

BEGIN_MESSAGE_MAP(SampleImageView, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

bool SampleImageView::LoadResourceImage(UINT resourceId)
{
    image_.Destroy();
    hasCandidate_ = false;
    const auto resource = FindResource(AfxGetResourceHandle(), MAKEINTRESOURCE(resourceId), RT_RCDATA);
    if (!resource) { Invalidate(); return false; }
    const auto bytes = LoadResource(AfxGetResourceHandle(), resource);
    CComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(static_cast<const BYTE*>(LockResource(bytes)),
        SizeofResource(AfxGetResourceHandle(), resource)));
    const bool loaded = stream && SUCCEEDED(image_.Load(stream));
    Invalidate();
    return loaded;
}

void SampleImageView::SetCandidateVisible(bool visible)
{
    hasCandidate_ = visible;
    Invalidate();
}

CSize SampleImageView::ImageSize() const
{
    return image_.IsNull() ? CSize(0, 0) : CSize(image_.GetWidth(), image_.GetHeight());
}

BOOL SampleImageView::OnEraseBkgnd(CDC*) { return TRUE; }

void SampleImageView::OnPaint()
{
    CPaintDC dc(this);
    CRect bounds;
    GetClientRect(bounds);
    dc.FillSolidRect(bounds, RGB(19, 28, 41));
    if (image_.IsNull()) {
        dc.SetBkMode(TRANSPARENT);
        dc.SetTextColor(RGB(196, 206, 220));
        dc.SelectObject(GetParent()->GetFont());
        dc.DrawText(L"샘플 선택", bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const int padding = MulDiv(32, GetDpiForWindow(m_hWnd), 96);
    const double scale = (std::min)(static_cast<double>((std::max)(1, bounds.Width() - padding)) / image_.GetWidth(),
        static_cast<double>((std::max)(1, bounds.Height() - padding)) / image_.GetHeight());
    const int width = static_cast<int>(image_.GetWidth() * scale);
    const int height = static_cast<int>(image_.GetHeight() * scale);
    CRect imageRect((bounds.Width() - width) / 2, (bounds.Height() - height) / 2, 0, 0);
    imageRect.right = imageRect.left + width;
    imageRect.bottom = imageRect.top + height;
    dc.SetStretchBltMode(HALFTONE);
    dc.SetBrushOrg(0, 0);
    image_.Draw(dc, imageRect);
    if (hasCandidate_) {
        // S03 fixture overlay: fixed demonstration rectangle, not a detector output.
        CRect candidate(imageRect.left + width * 43 / 100, imageRect.top + height / 8,
            imageRect.left + width * 65 / 100, imageRect.bottom - height / 8);
        CPen pen(PS_SOLID, MulDiv(2, GetDpiForWindow(m_hWnd), 96), RGB(255, 195, 67));
        auto oldPen = dc.SelectObject(&pen);
        auto oldBrush = dc.SelectStockObject(NULL_BRUSH);
        dc.Rectangle(candidate);
        dc.SelectObject(oldBrush);
        dc.SelectObject(oldPen);
    }
}
