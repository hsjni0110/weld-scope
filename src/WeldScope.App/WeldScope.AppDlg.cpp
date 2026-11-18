#include "pch.h"
#include "WeldScope.App.h"
#include "WeldScope.AppDlg.h"
#include "afxdialogex.h"
#include <algorithm>
#include <iterator>

namespace {
using weldscope::SampleState;
using weldscope::SampleVerdict;
struct SampleFixture {
    const wchar_t* label;
    const wchar_t* file;
    const wchar_t* displayName;
    UINT resource;
    SampleVerdict verdict;
};
const SampleFixture samples[] = {
    {L"정상 샘플", L"train/OK/image_1.png", nullptr, IDR_SAMPLE_NORMAL, SampleVerdict::Normal},
    {L"불량 샘플", L"train/KO/image_1.png", nullptr, IDR_SAMPLE_DEFECT, SampleVerdict::Defect},
    {L"검토 필요 샘플", L"train/KO/image_1.png", nullptr, IDR_SAMPLE_DEFECT, SampleVerdict::Review},
    {L"읽기 오류 샘플", L"corrupt.png", nullptr, IDR_SAMPLE_CORRUPT, SampleVerdict::None},
    {L"긴 파일명 샘플", L"train/OK/image_1.png", L"배터리_용접_외관_검사_긴_파일명_표시_확인_샘플_image_1.png", IDR_SAMPLE_NORMAL, SampleVerdict::Normal}
};
const wchar_t* VerdictText(SampleVerdict verdict)
{
    switch (verdict) {
    case SampleVerdict::Normal: return L"정상";
    case SampleVerdict::Defect: return L"불량";
    case SampleVerdict::Review: return L"검토 필요";
    default: return L"판정 대기";
    }
}
const wchar_t* StateText(SampleState state)
{
    switch (state) {
    case SampleState::Ready: return L"준비";
    case SampleState::Running: return L"검사 중";
    case SampleState::Complete: return L"완료";
    case SampleState::Cancelled: return L"중지";
    case SampleState::Error: return L"처리 오류";
    default: return L"대기";
    }
}
}

BEGIN_MESSAGE_MAP(CWeldScopeAppDlg, CDialogEx)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_CTLCOLOR()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_WM_VSCROLL()
    ON_WM_MOUSEWHEEL()
    ON_WM_TIMER()
    ON_WM_DESTROY()
    ON_MESSAGE(WM_DPICHANGED, OnDpiChanged)
    ON_CBN_SELCHANGE(IDC_SAMPLE_PICKER, OnSelectSample)
    ON_BN_CLICKED(IDC_START, OnStart)
    ON_BN_CLICKED(IDC_STOP, OnStop)
    ON_BN_CLICKED(IDC_CANDIDATE, OnToggleCandidate)
END_MESSAGE_MAP()

CWeldScopeAppDlg::CWeldScopeAppDlg(CWnd* parent) : CDialogEx(IDD, parent) {}
int CWeldScopeAppDlg::Dip(int value) const { return MulDiv(value, dpi_, 96); }

BOOL CWeldScopeAppDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    dpi_ = GetDpiForWindow(m_hWnd);
    SetIcon(AfxGetApp()->LoadIcon(IDR_MAINFRAME), TRUE);
    auto label = [this](CStatic& control, const wchar_t* text, UINT id, DWORD style = 0) {
        control.Create(text, WS_CHILD | WS_VISIBLE | SS_NOPREFIX | style, CRect(), this, id);
    };
    label(title_, L"WeldScope", IDC_TITLE);
    label(badge_, L"샘플", IDC_BADGE, SS_CENTER);
    label(section_, L"검사 콘솔", IDC_SECTION);
    label(imageTitle_, L"원본 이미지", IDC_IMAGE_TITLE);
    label(imageMeta_, L"입력 대기", IDC_IMAGE_META, SS_RIGHT);
    label(fileName_, L"샘플 선택", IDC_FILENAME);
    label(resultTitle_, L"샘플 판정", IDC_RESULT_TITLE);
    label(verdict_, L"판정 대기", IDC_VERDICT);
    label(statusTitle_, L"작업 상태", IDC_STATUS_TITLE);
    label(status_, L"대기", IDC_STATUS);
    label(detail_, L"샘플 선택", IDC_DETAIL);
    label(historyTitle_, L"샘플 이력", IDC_HISTORY_TITLE);
    label(historyNote_, L"현재 실행", IDC_HISTORY_NOTE, SS_RIGHT);
    label(footer_, L"OPENZDM  |  CC BY 4.0", IDC_FOOTER);
    samplePicker_.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        CRect(), this, IDC_SAMPLE_PICKER);
    for (const auto& sample : samples) samplePicker_.AddString(sample.label);
    start_.Create(L"검사 시작(&S)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, CRect(), this, IDC_START);
    stop_.Create(L"검사 중지(&T)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(), this, IDC_STOP);
    candidate_.Create(L"샘플 후보(&O)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(), this, IDC_CANDIDATE);
    candidate_.SetCheck(BST_CHECKED);
    const auto imageClass = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW, LoadCursor(nullptr, IDC_ARROW));
    image_.CreateEx(0, imageClass, L"원본 샘플 이미지", WS_CHILD | WS_VISIBLE,
        CRect(), this, IDC_SAMPLE_IMAGE);
    history_.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        CRect(), this, IDC_HISTORY);
    history_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    history_.SetBkColor(RGB(255, 255, 255));
    history_.SetTextBkColor(RGB(255, 255, 255));
    history_.SetTextColor(RGB(28, 39, 55));
    history_.InsertColumn(0, L"순서", LVCFMT_LEFT, Dip(64));
    history_.InsertColumn(1, L"시각", LVCFMT_LEFT, Dip(96));
    history_.InsertColumn(2, L"샘플", LVCFMT_LEFT, Dip(160));
    history_.InsertColumn(3, L"판정", LVCFMT_LEFT, Dip(130));
    history_.InsertColumn(4, L"원본 경로", LVCFMT_LEFT, Dip(300));
    SetFonts();
    // Fit the initial window to this monitor rather than extending below the taskbar.
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfo(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = (std::min)(Dip(1180), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    const int height = (std::min)(Dip(780), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    SetWindowPos(nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow();
    Refresh();
    samplePicker_.SetFocus();
    return FALSE;
}

void CWeldScopeAppDlg::SetFonts()
{
    auto create = [this](CFont& font, int height, int weight) {
        font.DeleteObject();
        font.CreateFont(-Dip(height), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"맑은 고딕");
    };
    create(bodyFont_, 14, FW_NORMAL);
    create(titleFont_, 25, FW_BOLD);
    create(verdictFont_, 32, FW_BOLD);
    create(labelFont_, 15, FW_BOLD);
    SetFont(&bodyFont_);
    for (CWnd* child = GetWindow(GW_CHILD); child; child = child->GetNextWindow()) child->SetFont(&bodyFont_);
    title_.SetFont(&titleFont_);
    verdict_.SetFont(&verdictFont_);
    for (auto label : {&imageTitle_, &resultTitle_, &historyTitle_, &section_}) label->SetFont(&labelFont_);
}

void CWeldScopeAppDlg::Layout()
{
    if (!history_.GetSafeHwnd() || isLayingOut_) return;
    isLayingOut_ = true;
    CRect bounds;
    GetClientRect(bounds);
    const int contentHeight = (std::max)(bounds.Height(), Dip(640));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMax = contentHeight - 1;
    scroll.nPage = bounds.Height();
    scroll.nPos = scrollOffset_;
    SetScrollInfo(SB_VERT, &scroll, TRUE);
    scrollOffset_ = GetScrollPos(SB_VERT);
    GetClientRect(bounds);
    const int margin = Dip(28), gap = Dip(20), rightWidth = Dip(284);
    const int top = Dip(152);
    const int panelBottom = (std::max)(top + Dip(288), contentHeight - Dip(264));
    imagePanel_ = CRect(margin, top, bounds.Width() - margin - gap - rightWidth, panelBottom);
    verdictPanel_ = CRect(imagePanel_.right + gap, top, bounds.Width() - margin, panelBottom);
    historyPanel_ = CRect(margin, panelBottom + gap, bounds.Width() - margin, contentHeight - Dip(44));
    auto move = [this](CWnd& control, int x, int y, int width, int height) {
        control.MoveWindow(x, y - scrollOffset_, (std::max)(1, width), (std::max)(1, height));
    };
    move(title_, margin, Dip(22), Dip(172), Dip(38));
    move(badge_, margin + Dip(184), Dip(30), Dip(58), Dip(25));
    move(section_, margin, Dip(88), Dip(96), Dip(28));
    move(samplePicker_, margin + Dip(110), Dip(84), Dip(320), Dip(260));
    move(start_, bounds.Width() - margin - Dip(240), Dip(83), Dip(116), Dip(36));
    move(stop_, bounds.Width() - margin - Dip(116), Dip(83), Dip(116), Dip(36));
    move(imageTitle_, imagePanel_.left + gap, top + Dip(18), Dip(150), Dip(25));
    move(imageMeta_, imagePanel_.left + Dip(174), top + Dip(20), imagePanel_.Width() - Dip(194), Dip(24));
    move(image_, imagePanel_.left + gap, top + Dip(58), imagePanel_.Width() - 2 * gap, imagePanel_.Height() - Dip(158));
    move(fileName_, imagePanel_.left + gap, panelBottom - Dip(88), imagePanel_.Width() - 2 * gap, Dip(72));
    const int rx = verdictPanel_.left + gap, rw = verdictPanel_.Width() - 2 * gap;
    move(resultTitle_, rx, top + Dip(20), rw, Dip(26));
    move(verdict_, rx, top + Dip(60), rw, Dip(50));
    move(statusTitle_, rx, top + Dip(137), Dip(90), Dip(24));
    move(status_, rx + Dip(96), top + Dip(137), rw - Dip(96), Dip(24));
    move(detail_, rx, top + Dip(176), rw, Dip(54));
    move(candidate_, rx, panelBottom - Dip(52), rw, Dip(32));
    move(historyTitle_, margin + gap, historyPanel_.top + Dip(15), Dip(150), Dip(26));
    move(historyNote_, historyPanel_.right - Dip(160), historyPanel_.top + Dip(16), Dip(140), Dip(24));
    move(history_, margin + gap, historyPanel_.top + Dip(50), historyPanel_.Width() - 2 * gap, historyPanel_.Height() - Dip(66));
    move(footer_, margin, contentHeight - Dip(30), bounds.Width() - 2 * margin, Dip(24));
    const int widths[] = {56, 92, 148, 116};
    int fixedWidth = 0;
    for (int index = 0; index < 4; ++index) { history_.SetColumnWidth(index, Dip(widths[index])); fixedWidth += Dip(widths[index]); }
    history_.SetColumnWidth(4, (std::max)(Dip(160), historyPanel_.Width() - 2 * gap - fixedWidth - Dip(22)));
    Invalidate(FALSE);
    isLayingOut_ = false;
}

void CWeldScopeAppDlg::Refresh()
{
    const auto state = session_.state();
    const bool running = state == SampleState::Running;
    start_.EnableWindow(state == SampleState::Ready || state == SampleState::Complete || state == SampleState::Cancelled);
    stop_.EnableWindow(running);
    samplePicker_.EnableWindow(!running);
    verdict_.SetWindowText(VerdictText(session_.verdict()));
    status_.SetWindowText(StateText(state));
    const wchar_t* detail = L"샘플 선택";
    switch (state) {
    case SampleState::Ready: detail = L"검사 시작"; break;
    case SampleState::Running: detail = L"샘플 실행 중"; break;
    case SampleState::Complete: detail = L"사전 지정 결과"; break;
    case SampleState::Cancelled: detail = L"판정 없음\n검사 재시작"; break;
    case SampleState::Error: detail = L"이미지 읽기 실패\n다른 샘플 선택"; break;
    default: break;
    }
    detail_.SetWindowText(detail);
    const bool showCandidate = session_.verdict() == SampleVerdict::Defect || session_.verdict() == SampleVerdict::Review;
    candidate_.EnableWindow(showCandidate);
    image_.SetCandidateVisible(showCandidate && candidate_.GetCheck() == BST_CHECKED);
    // A disabled focused button must not trap the keyboard after completion/stop.
    const HWND focus = ::GetFocus();
    if (focus && !::IsWindowEnabled(focus)) {
        (running ? static_cast<CWnd*>(&stop_) : static_cast<CWnd*>(&samplePicker_))->SetFocus();
    }
    Invalidate(FALSE);
}

void CWeldScopeAppDlg::OnSelectSample()
{
    if (session_.state() == SampleState::Running) return;
    selectedSample_ = samplePicker_.GetCurSel();
    if (selectedSample_ < 0 || selectedSample_ >= static_cast<int>(std::size(samples))) return;
    const auto& sample = samples[selectedSample_];
    const bool loaded = image_.LoadResourceImage(sample.resource);
    session_.select(sample.verdict, loaded);
    CString fileText;
    if (sample.displayName) fileText.Format(L"%s\n%s", sample.displayName, sample.file);
    else fileText = sample.file;
    fileName_.SetWindowText(fileText);
    CString metadata;
    const auto size = image_.ImageSize();
    if (loaded) metadata.Format(L"%d × %d px", size.cx, size.cy);
    else metadata = L"읽기 오류";
    imageMeta_.SetWindowText(metadata);
    Refresh();
}

void CWeldScopeAppDlg::OnStart()
{
    if (!session_.start()) return;
    // Each run has its own timer ID so an old queued WM_TIMER cannot complete a new run.
    completionTimer_ = SetTimer(nextTimerId_++, 1200, nullptr);
    if (!completionTimer_) { session_.cancel(); Refresh(); return; }
    Refresh();
}

void CWeldScopeAppDlg::OnStop()
{
    if (completionTimer_) KillTimer(completionTimer_);
    completionTimer_ = 0;
    session_.cancel();
    Refresh();
}

void CWeldScopeAppDlg::OnToggleCandidate() { Refresh(); }
void CWeldScopeAppDlg::OnOK() { OnStart(); }
BOOL CWeldScopeAppDlg::PreTranslateMessage(MSG* message)
{
    const BOOL handled = CDialogEx::PreTranslateMessage(message);
    if (message->message == WM_KEYDOWN && message->wParam == VK_TAB) {
        CWnd* focus = GetFocus();
        if (focus && IsChild(focus)) {
            CRect target, viewport;
            focus->GetWindowRect(target);
            ScreenToClient(target);
            GetClientRect(viewport);
            int next = scrollOffset_;
            if (target.top < 0) next += target.top - Dip(12);
            else if (target.bottom > viewport.bottom) next += target.bottom - viewport.bottom + Dip(12);
            SetScrollPos(SB_VERT, next, TRUE);
            scrollOffset_ = GetScrollPos(SB_VERT);
            Layout();
        }
    }
    return handled;
}

void CWeldScopeAppDlg::OnTimer(UINT_PTR id)
{
    if (id != completionTimer_ || completionTimer_ == 0) { CDialogEx::OnTimer(id); return; }
    KillTimer(completionTimer_);
    completionTimer_ = 0;
    if (session_.complete()) AddHistory();
    Refresh();
}

void CWeldScopeAppDlg::AddHistory()
{
    CString sequence;
    sequence.Format(L"%d", history_.GetItemCount() + 1);
    const int row = history_.InsertItem(0, sequence);
    history_.SetItemText(row, 1, CTime::GetCurrentTime().Format(L"%H:%M:%S"));
    history_.SetItemText(row, 2, samples[selectedSample_].label);
    history_.SetItemText(row, 3, VerdictText(session_.verdict()));
    history_.SetItemText(row, 4, samples[selectedSample_].file);
}

BOOL CWeldScopeAppDlg::OnEraseBkgnd(CDC*) { return TRUE; }
void CWeldScopeAppDlg::OnPaint()
{
    CPaintDC dc(this);
    CRect bounds;
    GetClientRect(bounds);
    dc.FillSolidRect(bounds, RGB(243, 246, 250));
    dc.FillSolidRect(0, -scrollOffset_, bounds.Width(), Dip(72), RGB(255, 255, 255));
    dc.FillSolidRect(0, Dip(71) - scrollOffset_, bounds.Width(), Dip(1), RGB(221, 227, 235));
    for (auto panel : {imagePanel_, verdictPanel_, historyPanel_}) {
        panel.OffsetRect(0, -scrollOffset_);
        dc.FillSolidRect(panel, RGB(255, 255, 255));
        dc.Draw3dRect(panel, RGB(221, 227, 235), RGB(221, 227, 235));
    }
}

HBRUSH CWeldScopeAppDlg::OnCtlColor(CDC* dc, CWnd* wnd, UINT controlType)
{
    const auto brush = CDialogEx::OnCtlColor(dc, wnd, controlType);
    if (controlType == CTLCOLOR_STATIC || controlType == CTLCOLOR_BTN) {
        dc->SetTextColor(RGB(28, 39, 55));
        dc->SetBkMode(TRANSPARENT);
        const int id = wnd->GetDlgCtrlID();
        if (id == IDC_VERDICT) {
            switch (session_.verdict()) {
            case SampleVerdict::Normal: dc->SetTextColor(RGB(0, 111, 85)); break;
            case SampleVerdict::Defect: dc->SetTextColor(RGB(181, 42, 50)); break;
            case SampleVerdict::Review: dc->SetTextColor(RGB(139, 87, 0)); break;
            default: dc->SetTextColor(RGB(83, 99, 120)); break;
            }
        }
        if (id == IDC_BADGE) { dc->SetBkColor(RGB(231, 240, 252)); return static_cast<HBRUSH>(backgroundBrush_.GetSafeHandle()); }
        const bool background = id == IDC_SECTION || id == IDC_FOOTER;
        dc->SetBkColor(background ? RGB(243, 246, 250) : RGB(255, 255, 255));
        return static_cast<HBRUSH>((background ? backgroundBrush_ : whiteBrush_).GetSafeHandle());
    }
    return brush;
}

void CWeldScopeAppDlg::OnSize(UINT type, int width, int height)
{
    CDialogEx::OnSize(type, width, height);
    if (type != SIZE_MINIMIZED) Layout();
}
void CWeldScopeAppDlg::OnGetMinMaxInfo(MINMAXINFO* info)
{
    CDialogEx::OnGetMinMaxInfo(info);
    info->ptMinTrackSize = CPoint(Dip(760), Dip(420));
}
void CWeldScopeAppDlg::OnVScroll(UINT code, UINT position, CScrollBar* bar)
{
    if (bar) { CDialogEx::OnVScroll(code, position, bar); return; }
    SCROLLINFO info{sizeof(info), SIF_ALL};
    GetScrollInfo(SB_VERT, &info);
    int next = scrollOffset_;
    switch (code) {
    case SB_LINEUP: next -= Dip(40); break;
    case SB_LINEDOWN: next += Dip(40); break;
    case SB_PAGEUP: next -= info.nPage; break;
    case SB_PAGEDOWN: next += info.nPage; break;
    case SB_THUMBTRACK: next = info.nTrackPos; break;
    case SB_TOP: next = 0; break;
    case SB_BOTTOM: next = info.nMax; break;
    default: return;
    }
    SetScrollPos(SB_VERT, next, TRUE);
    scrollOffset_ = GetScrollPos(SB_VERT);
    Layout();
}
BOOL CWeldScopeAppDlg::OnMouseWheel(UINT, short delta, CPoint)
{
    SetScrollPos(SB_VERT, scrollOffset_ - MulDiv(delta, Dip(80), WHEEL_DELTA), TRUE);
    scrollOffset_ = GetScrollPos(SB_VERT);
    Layout();
    return TRUE;
}
LRESULT CWeldScopeAppDlg::OnDpiChanged(WPARAM dpi, LPARAM suggestedRect)
{
    dpi_ = HIWORD(dpi);
    const auto rect = reinterpret_cast<RECT*>(suggestedRect);
    SetWindowPos(nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
    SetFonts();
    Layout();
    return 0;
}
void CWeldScopeAppDlg::OnDestroy()
{
    if (completionTimer_) KillTimer(completionTimer_);
    CDialogEx::OnDestroy();
}
