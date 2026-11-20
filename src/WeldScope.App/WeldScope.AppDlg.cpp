#include "pch.h"
#include "WeldScope.App.h"
#include "WeldScope.AppDlg.h"
#include "afxdialogex.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <opencv2/imgproc.hpp>

namespace {
using namespace weldscope;
constexpr UINT_PTR inspectionTimerId = 1;

#ifndef WELDSCOPE_S10
using State = InspectionJobState;

const wchar_t* VerdictText(const std::optional<BaselineDecision>& verdict)
{
    if (!verdict) return L"판정 대기";
    return *verdict == BaselineDecision::Pass ? L"정상" : L"불량";
}

const wchar_t* StateText(State state)
{
    switch (state) {
    case State::Ready: return L"준비";
    case State::Running: return L"검사 중";
    case State::Complete: return L"완료";
    case State::Cancelled: return L"중지";
    case State::Error: return L"처리 오류";
    case State::Closing: return L"종료 대기";
    default: return L"대기";
    }
}
#endif

std::filesystem::path BaselineConfigPath()
{
    std::wstring executable(32768, L'\0');
    const DWORD capacity = static_cast<DWORD>(executable.size());
    const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(), capacity);
    if (!length || length >= capacity) throw std::runtime_error("Executable path unavailable");
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"baseline.json";
}

#ifdef WELDSCOPE_S10
const wchar_t* StoredDecision(InspectionWorkState workState, std::string_view decision) {
    if (workState == InspectionWorkState::Cancelled) return L"중지";
    if (decision == "PASS") return L"정상";
    if (decision == "FAIL") return L"불량";
    if (decision == "REVIEW") return L"검토 필요";
    return L"처리 오류";
}

std::optional<BaselineDecision> DisplayDecision(const std::string& decision) {
    if (decision == "PASS") return BaselineDecision::Pass;
    if (decision == "FAIL") return BaselineDecision::Fail;
    return std::nullopt;
}

bool IntegrationBusy(IntegrationState state) {
    return state == IntegrationState::Connecting || state == IntegrationState::Inspecting ||
           state == IntegrationState::Saving;
}

bool PendingSave(IntegrationState state) {
    return state == IntegrationState::Unsaved || state == IntegrationState::CommitUnknown ||
           state == IntegrationState::Conflict;
}

std::wstring WideAscii(const std::string& text) { return {text.begin(), text.end()}; }
#endif
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
    ON_WM_CLOSE()
    ON_WM_DESTROY()
    ON_MESSAGE(WM_DPICHANGED, OnDpiChanged)
    ON_BN_CLICKED(IDC_FILE_PICKER, OnSelectFile)
    ON_BN_CLICKED(IDC_START, OnStart)
    ON_BN_CLICKED(IDC_STOP, OnStop)
    ON_BN_CLICKED(IDC_CANDIDATE, OnToggleCandidate)
#ifdef WELDSCOPE_S10
    ON_BN_CLICKED(IDC_SAVE_RETRY, OnRetrySave)
    ON_BN_CLICKED(IDC_HISTORY_REFRESH, OnRefreshHistory)
    ON_BN_CLICKED(IDC_HISTORY_OPEN, OnOpenHistory)
#endif
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
    label(section_, L"검사 콘솔", IDC_SECTION);
    label(imageTitle_, L"원본 이미지", IDC_IMAGE_TITLE);
    label(imageMeta_, L"입력 대기", IDC_IMAGE_META, SS_RIGHT);
    label(fileName_, L"이미지 선택", IDC_FILENAME, SS_PATHELLIPSIS);
    label(resultTitle_, L"검사 판정", IDC_RESULT_TITLE);
    label(verdict_, L"판정 대기", IDC_VERDICT);
    label(statusTitle_, L"작업 상태", IDC_STATUS_TITLE);
    label(status_, L"대기", IDC_STATUS);
    label(detail_, L"이미지 선택", IDC_DETAIL);
    label(historyTitle_, L"검사 이력", IDC_HISTORY_TITLE);
    label(historyNote_, L"현재 실행", IDC_HISTORY_NOTE, SS_RIGHT);
    filePicker_.Create(L"이미지 선택(&I)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        CRect(), this, IDC_FILE_PICKER);
    start_.Create(L"검사 시작(&S)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, CRect(), this, IDC_START);
    stop_.Create(L"검사 중지(&T)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(), this, IDC_STOP);
    candidate_.Create(L"이상 후보(&O)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, CRect(), this, IDC_CANDIDATE);
    candidate_.SetCheck(BST_CHECKED);
    const auto imageClass = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW, LoadCursor(nullptr, IDC_ARROW));
    image_.CreateEx(0, imageClass, L"원본 이미지", WS_CHILD | WS_VISIBLE,
        CRect(), this, IDC_INSPECTION_IMAGE);
    history_.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        CRect(), this, IDC_HISTORY);
    history_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    history_.SetBkColor(RGB(255, 255, 255));
    history_.SetTextBkColor(RGB(255, 255, 255));
    history_.SetTextColor(RGB(28, 39, 55));
    history_.InsertColumn(0, L"순서", LVCFMT_LEFT, Dip(64));
    history_.InsertColumn(1, L"시각", LVCFMT_LEFT, Dip(96));
    history_.InsertColumn(2, L"파일", LVCFMT_LEFT, Dip(148));
    history_.InsertColumn(3, L"판정", LVCFMT_LEFT, Dip(130));
    history_.InsertColumn(4, L"원본 경로", LVCFMT_LEFT, Dip(300));
#ifdef WELDSCOPE_S10
    label(connectionState_, L"가상 장비 연결 대기", IDC_CONNECTION_STATE);
    label(saveState_, L"DB 연결 확인", IDC_SAVE_STATE);
    retrySave_.Create(L"재연결", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(), this, IDC_SAVE_RETRY);
    refreshHistory_.Create(L"이력 새로 고침", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        CRect(), this, IDC_HISTORY_REFRESH);
    openHistory_.Create(L"이력 열기", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        CRect(), this, IDC_HISTORY_OPEN);
    LVCOLUMN idColumn{};
    wchar_t idHeader[] = L"검사 ID";
    idColumn.mask = LVCF_TEXT;
    idColumn.pszText = idHeader;
    history_.SetColumn(0, &idColumn);
    try {
        const auto config = BaselineConfigPath();
        integration_ = std::make_unique<IntegratedInspection>(config, config.parent_path() / L"defect.png", 33361);
        pollTimer_ = SetTimer(inspectionTimerId, 25, nullptr);
        if (!pollTimer_) throw std::runtime_error("Inspection timer unavailable");
    } catch (const std::exception&) {
        detail_.SetWindowText(L"검사 준비 실패");
    }
#endif
    SetFonts();
    // Fit the initial window to this monitor rather than extending below the taskbar.
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfo(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = (std::min)(Dip(1180), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    const int height = (std::min)(Dip(780), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    SetWindowPos(nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow();
    Refresh();
    filePicker_.SetFocus();
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
    move(section_, margin, Dip(88), Dip(96), Dip(28));
    move(filePicker_, margin + Dip(110), Dip(83), Dip(160), Dip(36));
    move(start_, bounds.Width() - margin - Dip(240), Dip(83), Dip(116), Dip(36));
    move(stop_, bounds.Width() - margin - Dip(116), Dip(83), Dip(116), Dip(36));
    move(imageTitle_, imagePanel_.left + gap, top + Dip(18), Dip(150), Dip(25));
    move(imageMeta_, imagePanel_.left + Dip(174), top + Dip(20), imagePanel_.Width() - Dip(194), Dip(24));
    move(image_, imagePanel_.left + gap, top + Dip(58), imagePanel_.Width() - 2 * gap, imagePanel_.Height() - Dip(158));
    move(fileName_, imagePanel_.left + gap, panelBottom - Dip(72), imagePanel_.Width() - 2 * gap, Dip(24));
    const int rx = verdictPanel_.left + gap, rw = verdictPanel_.Width() - 2 * gap;
    move(resultTitle_, rx, top + Dip(20), rw, Dip(26));
    move(verdict_, rx, top + Dip(60), rw, Dip(50));
    move(statusTitle_, rx, top + Dip(137), Dip(90), Dip(24));
    move(status_, rx + Dip(96), top + Dip(137), rw - Dip(96), Dip(24));
    move(detail_, rx, top + Dip(176), rw, Dip(62));
    move(candidate_, rx, panelBottom - Dip(52), rw, Dip(32));
    move(historyTitle_, margin + gap, historyPanel_.top + Dip(15), Dip(150), Dip(26));
    move(historyNote_, historyPanel_.right - Dip(160), historyPanel_.top + Dip(16), Dip(140), Dip(24));
    move(history_, margin + gap, historyPanel_.top + Dip(50), historyPanel_.Width() - 2 * gap, historyPanel_.Height() - Dip(66));
    const int widths[] = {56, 92, 148, 116};
    int fixedWidth = 0;
    for (int index = 0; index < 4; ++index) { history_.SetColumnWidth(index, Dip(widths[index])); fixedWidth += Dip(widths[index]); }
    history_.SetColumnWidth(4, (std::max)(Dip(160), historyPanel_.Width() - 2 * gap - fixedWidth - Dip(22)));
#ifdef WELDSCOPE_S10
    move(connectionState_, margin + Dip(178), Dip(33), Dip(225), Dip(24));
    move(saveState_, bounds.Width() - margin - Dip(254), Dip(33), Dip(140), Dip(24));
    move(retrySave_, bounds.Width() - margin - Dip(110), Dip(24), Dip(110), Dip(36));
    move(openHistory_, historyPanel_.right - Dip(105), historyPanel_.top + Dip(10), Dip(85), Dip(30));
    move(refreshHistory_, historyPanel_.right - Dip(239), historyPanel_.top + Dip(10), Dip(125), Dip(30));
    move(historyNote_, historyPanel_.right - Dip(440), historyPanel_.top + Dip(16), Dip(190), Dip(24));
    history_.SetColumnWidth(0, Dip(220));
#endif
    Invalidate(FALSE);
    isLayingOut_ = false;
}

void CWeldScopeAppDlg::Refresh()
{
#ifdef WELDSCOPE_S10
    RefreshIntegration();
    const bool closing = closeRequested_;
#else
    const auto state = job_.state();
    const bool running = state == State::Running;
    const bool closing = state == State::Closing;
    start_.EnableWindow(!closing && !job_.busy() && !job_.selected_path().empty());
    stop_.EnableWindow(running);
    filePicker_.EnableWindow(!running && !closing);
    verdict_.SetWindowText(VerdictText(verdictValue_));
    status_.SetWindowText(StateText(state));
#endif
    const bool hasCandidate = image_.HasCandidate() && !closing;
    candidate_.EnableWindow(hasCandidate);
    image_.SetCandidateVisible(hasCandidate && candidate_.GetCheck() == BST_CHECKED);
#ifndef WELDSCOPE_S10
    const HWND focus = ::GetFocus();
    if (!closing && focus && !::IsWindowEnabled(focus)) {
        (running ? static_cast<CWnd*>(&stop_) : static_cast<CWnd*>(&filePicker_))->SetFocus();
    }
#endif
    Invalidate(FALSE);
}

void CWeldScopeAppDlg::ClearResult()
{
    verdictValue_.reset();
#ifdef WELDSCOPE_S10
    verdict_.SetWindowText(L"판정 대기");
#endif
    image_.SetImages({}, {});
    imageMeta_.SetWindowText(L"");
    detail_.SetWindowText(L"");
    candidate_.SetCheck(BST_CHECKED);
}

void CWeldScopeAppDlg::OnSelectFile()
{
#ifdef WELDSCOPE_S10
    if (closeRequested_ || !integration_ || IntegrationBusy(integration_->Snapshot().state)) return;
#else
    if (job_.state() == State::Running || job_.state() == State::Closing) return;
#endif
    CFileDialog picker(TRUE, L"png", nullptr, OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR,
        L"PNG 이미지 (*.png)|*.png||", this, 0, FALSE);
    if (picker.DoModal() != IDOK) return;
    const std::filesystem::path path(picker.GetPathName().GetString());
#ifdef WELDSCOPE_S10
    if (closeRequested_) return;
    selectedFile_ = path;
    view_ = InspectionView::FileSelection;
    historyOpenRevision_ = (std::numeric_limits<std::uint64_t>::max)();
#else
    if (!job_.select(path)) return;
#endif
    ClearResult();
    fileName_.SetWindowText(path.c_str());
    Refresh();
}

void CWeldScopeAppDlg::OnStart()
{
#ifdef WELDSCOPE_S10
    if (closeRequested_ || !integration_ || !pollTimer_) return;
    try {
        if (integration_->StartFile(selectedFile_)) {
            view_ = InspectionView::Live;
            ClearResult();
        }
    } catch (const std::exception&) { detail_.SetWindowText(L"검사 시작 실패"); }
    Refresh();
#else
    if (job_.busy() || job_.selected_path().empty() || job_.state() == State::Closing) return;
    ClearResult();
    if (!pollTimer_) pollTimer_ = SetTimer(inspectionTimerId, 25, nullptr);
    bool started = false;
    if (pollTimer_) {
        try {
            started = job_.start(BaselineConfigPath());
        } catch (const std::exception&) {
            // A failed executable-path lookup leaves no worker to drain.
        }
    }
    if (!started) {
        if (pollTimer_) KillTimer(pollTimer_);
        pollTimer_ = 0;
        detail_.SetWindowText(L"검사 시작 실패");
    }
    Refresh();
    if (!started) status_.SetWindowText(L"처리 오류");
#endif
}

void CWeldScopeAppDlg::OnStop()
{
#ifdef WELDSCOPE_S10
    if (integration_) integration_->CancelInspection();
#else
    if (job_.state() != State::Running) return;
    job_.cancel();
    ClearResult();
    Refresh();
#endif
}

void CWeldScopeAppDlg::OnCancel()
{
#ifdef WELDSCOPE_S10
    if (!integration_ || !pollTimer_) { CDialogEx::OnCancel(); return; }
    closeRequested_ = true;
    integration_->Pause(true);
    integration_->CancelInspection();
    Refresh();
#else
    job_.request_close();
    if (job_.busy()) {
        Refresh();
        return;
    }
    CDialogEx::OnCancel();
#endif
}

void CWeldScopeAppDlg::OnClose() { OnCancel(); }
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
    if (id != pollTimer_ || !pollTimer_) {
        CDialogEx::OnTimer(id);
        return;
    }
#ifdef WELDSCOPE_S10
    PollIntegration();
#else
    const auto result = job_.poll();
    if (job_.busy()) return;
    KillTimer(pollTimer_);
    pollTimer_ = 0;
    if (job_.state() == State::Closing) {
        CDialogEx::OnCancel();
        return;
    }
    if (result) ShowInspection(*result);
    Refresh();
#endif
}

void CWeldScopeAppDlg::ShowInspection(const FileInspectionResult& result)
{
        if (const auto* inspected = std::get_if<InspectedFile>(&result)) {
            verdictValue_ = inspected->decision;
            image_.SetImages(inspected->original, inspected->overlay);
            CString metadata, details;
            metadata.Format(L"%d × %d px", inspected->original.cols, inspected->original.rows);
            details.Format(L"후보 점수 %.4f\n읽기 %.2f ms\n분석 %.2f ms",
                inspected->score, inspected->loadMs, inspected->analysisMs);
            imageMeta_.SetWindowText(metadata);
            detail_.SetWindowText(details);
#ifndef WELDSCOPE_S10
            AddHistory(*inspected);
#endif
        } else if (const auto* error = std::get_if<FileInspectionFailure>(&result)) {
            switch (*error) {
            case FileInspectionFailure::Configuration:
                detail_.SetWindowText(L"기준 설정 오류\nbaseline.json 확인");
                break;
            case FileInspectionFailure::FileAccess:
                detail_.SetWindowText(L"파일 접근 실패\n경로와 권한 확인");
                break;
            case FileInspectionFailure::SizeLimit:
                detail_.SetWindowText(L"크기 한도 초과\n16 MiB, 1,600만 픽셀");
                break;
            case FileInspectionFailure::DamagedPng:
                detail_.SetWindowText(L"PNG 손상\n다른 이미지 선택");
                break;
            case FileInspectionFailure::UnsupportedFormat:
                detail_.SetWindowText(L"지원하지 않는 형식\n8비트 컬러 PNG 선택");
                break;
            case FileInspectionFailure::Analysis:
                detail_.SetWindowText(L"분석 실패\n검사 재시작");
                break;
            }
        }
}

#ifndef WELDSCOPE_S10
void CWeldScopeAppDlg::AddHistory(const InspectedFile& inspected)
{
    CString sequence;
    sequence.Format(L"%d", history_.GetItemCount() + 1);
    const int row = history_.InsertItem(0, sequence);
    if (row < 0) return;
    history_.SetItemText(row, 1, CTime::GetCurrentTime().Format(L"%H:%M:%S"));
    history_.SetItemText(row, 2, inspected.path.filename().c_str());
    history_.SetItemText(row, 3, VerdictText(inspected.decision));
    history_.SetItemText(row, 4, inspected.path.c_str());
}
#endif

#ifdef WELDSCOPE_S10
void CWeldScopeAppDlg::RefreshIntegration()
{
    const auto snapshot = integration_ ? integration_->Snapshot() : IntegrationSnapshot{};
    const bool ready = snapshot.state == IntegrationState::Ready || snapshot.state == IntegrationState::Stored;
    const bool inspecting = snapshot.state == IntegrationState::Inspecting;
    const bool pending = PendingSave(snapshot.state);
    start_.EnableWindow(!closeRequested_ && ready && !selectedFile_.empty());
    filePicker_.EnableWindow(!closeRequested_ && integration_ && !IntegrationBusy(snapshot.state) && !pending);
    stop_.EnableWindow(!closeRequested_ && inspecting);
    retrySave_.EnableWindow(!closeRequested_ && (pending || snapshot.state == IntegrationState::Disconnected));
    retrySave_.SetWindowText(pending ? L"저장 재시도" : L"재연결");
    refreshHistory_.EnableWindow(!closeRequested_ && ready && !historyRefresh_.valid());
    openHistory_.EnableWindow(!closeRequested_ && ready && !historyRows_.empty() && !historyOpen_.valid());
    const wchar_t* task = L"대기";
    const wchar_t* storage = L"DB 연결 확인";
    switch (snapshot.state) {
    case IntegrationState::Disconnected: storage = L"DB 연결 끊김"; break;
    case IntegrationState::Ready: storage = L"DB 연결"; task = L"준비"; break;
    case IntegrationState::Inspecting: storage = L"DB 연결"; task = L"검사 중"; break;
    case IntegrationState::Saving: storage = L"저장 중"; task = L"완료"; break;
    case IntegrationState::Stored: storage = L"저장 완료"; task = L"완료"; break;
    case IntegrationState::Unsaved: storage = L"미저장"; task = L"완료"; break;
    case IntegrationState::CommitUnknown: storage = L"저장 확인 필요"; task = L"완료"; break;
    case IntegrationState::Conflict: storage = L"저장 ID 충돌"; task = L"완료"; break;
    case IntegrationState::Connecting: break;
    }
    if (view_ == InspectionView::History) {
        task = historyTask_;
    } else if (view_ == InspectionView::FileSelection) {
        task = inspecting ? L"장비 검사 중" : L"준비";
        if (snapshot.state == IntegrationState::Stored) storage = L"DB 연결";
    } else if (snapshot.result) {
        if (std::holds_alternative<InspectionCancelled>(*snapshot.result)) task = L"중지";
        if (std::holds_alternative<FileInspectionFailure>(*snapshot.result)) task = L"처리 오류";
    }
    status_.SetWindowText(closeRequested_ ? L"종료 대기" : task);
    saveState_.SetWindowText(storage);
    if (snapshot.listenerFailed) connectionState_.SetWindowText(L"가상 장비 연결 오류");
    else if (snapshot.port) {
        CString port;
        port.Format(L"가상 장비 127.0.0.1:%u", static_cast<unsigned int>(snapshot.port));
        connectionState_.SetWindowText(port);
    }
}

void CWeldScopeAppDlg::OnRetrySave()
{
    if (integration_ && !closeRequested_) integration_->Retry();
}

void CWeldScopeAppDlg::OnRefreshHistory()
{
    if (closeRequested_ || historyRefresh_.valid()) return;
    try {
        historyRefresh_ = std::async(std::launch::async, [] {
            return InspectionHistory(HistoryConnectionFromEnvironment()).Recent();
        });
        historyNote_.SetWindowText(L"이력 조회 중");
    } catch (const std::exception&) { historyNote_.SetWindowText(L"이력 조회 실패"); }
}

void CWeldScopeAppDlg::OnOpenHistory()
{
    if (closeRequested_ || !integration_ || historyOpen_.valid()) return;
    const int row = history_.GetNextItem(-1, LVNI_SELECTED);
    if (row < 0 || static_cast<std::size_t>(row) >= historyRows_.size()) return;
    historyOpenRevision_ = integration_->Snapshot().revision;
    const auto inspectionId = historyRows_[static_cast<std::size_t>(row)].inspectionId;
    try {
        historyOpen_ = std::async(std::launch::async, [inspectionId] {
            OpenedInspection opened;
            auto stored = InspectionHistory(HistoryConnectionFromEnvironment()).Get(inspectionId);
            if (!stored) throw std::runtime_error("Inspection history row unavailable");
            opened.record = std::move(*stored);
            const auto& record = opened.record.record;
            try {
                const auto bytes = read_bytes(std::filesystem::u8path(record.sourcePath), image_file_limit);
                if (record.sourceSha256.empty()) opened.sourceStatus = L"원본 확인 불가";
                else if (sha256(bytes) != record.sourceSha256) opened.sourceStatus = L"원본 변경";
                else cv::cvtColor(decode_png(bytes), opened.image, cv::COLOR_BGR2BGRA);
            } catch (const InspectionInputError& error) {
                opened.sourceStatus = error.reason() == FileInspectionFailure::FileAccess ? L"원본 누락" : L"원본 열기 실패";
            } catch (const std::exception&) { opened.sourceStatus = L"원본 열기 실패"; }
            return opened;
        });
    } catch (const std::exception&) { historyNote_.SetWindowText(L"이력 열기 실패"); }
    Refresh();
}

void CWeldScopeAppDlg::ShowHistory(std::vector<InspectionSummary> rows)
{
    history_.DeleteAllItems();
    historyRows_ = std::move(rows);
    for (const auto& record : historyRows_) {
        const int row = history_.InsertItem(history_.GetItemCount(), WideAscii(record.inspectionId).c_str());
        if (row < 0) break;
        const auto path = std::filesystem::u8path(record.sourcePath);
        history_.SetItemText(row, 1, WideAscii(record.createdAtUtc.substr(11, 8)).c_str());
        history_.SetItemText(row, 2, path.filename().c_str());
        history_.SetItemText(row, 3, StoredDecision(record.workState, record.decision));
        history_.SetItemText(row, 4, path.c_str());
    }
    historyNote_.SetWindowText(historyRows_.empty() ? L"저장 이력 없음" : L"최근 저장 이력");
}

void CWeldScopeAppDlg::ShowOpenedInspection(const OpenedInspection& opened)
{
    selectedFile_.clear();
    ClearResult();
    const auto& record = opened.record.record;
    view_ = InspectionView::History;
    historyTask_ = record.workState == InspectionWorkState::Cancelled ? L"중지" :
        record.decision == "ERROR" ? L"처리 오류" : L"완료";
    verdictValue_ = DisplayDecision(record.decision);
    verdict_.SetWindowText(StoredDecision(record.workState, record.decision));
    fileName_.SetWindowText(std::filesystem::u8path(record.sourcePath).c_str());
    image_.SetImages(opened.image, {});
    imageMeta_.SetWindowText(opened.sourceStatus);
    CString details;
    if (!record.attempts.empty() && record.attempts.front().score) {
        const auto& attempt = record.attempts.front();
        details.Format(L"후보 점수 %.4f\n분석 %.2f ms\n%s",
            *attempt.score, attempt.durationMs, WideAscii(record.algorithmVersion).c_str());
    } else details = WideAscii(record.algorithmVersion).c_str();
    detail_.SetWindowText(details);
}

void CWeldScopeAppDlg::PollIntegration()
{
    const auto snapshot = integration_->Snapshot();
    const bool updated = snapshot.revision != integrationRevision_;
    bool displayChanged = false;
    const bool fromDevice = snapshot.record.requestId.compare(0, 5, "file-") != 0;
    if (!closeRequested_ && view_ == InspectionView::PendingDeviceSave &&
        (snapshot.state == IntegrationState::Stored || shownInspection_ != snapshot.record.inspectionId)) {
        view_ = InspectionView::FileSelection;
        ClearResult();
        fileName_.SetWindowText(selectedFile_.c_str());
        displayChanged = true;
    }
    if (!closeRequested_ && view_ == InspectionView::FileSelection && fromDevice &&
        PendingSave(snapshot.state) && snapshot.result) {
        view_ = InspectionView::PendingDeviceSave;
        ShowCurrentInspection(snapshot);
        displayChanged = true;
    }
    if (updated) {
        integrationRevision_ = snapshot.revision;
        if (snapshot.state == IntegrationState::Ready || snapshot.state == IntegrationState::Stored)
            refreshHistoryPending_ = true;
        if (!closeRequested_ && !snapshot.record.inspectionId.empty() && shownInspection_ != snapshot.record.inspectionId) {
            const bool preserveSelection = fromDevice && view_ == InspectionView::FileSelection;
            if (!preserveSelection) {
                if (fromDevice) selectedFile_.clear();
                view_ = InspectionView::Live;
                ShowCurrentInspection(snapshot);
            } else if (snapshot.result) {
                shownInspection_ = snapshot.record.inspectionId;
            }
        }
    }
    using namespace std::chrono_literals;
    if (historyRefresh_.valid() && historyRefresh_.wait_for(0ms) == std::future_status::ready) {
        try {
            auto rows = historyRefresh_.get();
            if (!closeRequested_) ShowHistory(std::move(rows));
        } catch (const std::exception&) { historyNote_.SetWindowText(L"이력 조회 실패"); }
        Refresh();
    }
    if (historyOpen_.valid() && historyOpen_.wait_for(0ms) == std::future_status::ready) {
        try {
            const auto opened = historyOpen_.get();
            if (!closeRequested_ && snapshot.revision == historyOpenRevision_) ShowOpenedInspection(opened);
        } catch (const std::exception&) { historyNote_.SetWindowText(L"이력 열기 실패"); }
        Refresh();
    }
    if (refreshHistoryPending_ && !closeRequested_ && !historyRefresh_.valid() &&
        (snapshot.state == IntegrationState::Ready || snapshot.state == IntegrationState::Stored)) {
        refreshHistoryPending_ = false;
        OnRefreshHistory();
    }
    if (closeRequested_ && !closingIntegration_ && !IntegrationBusy(snapshot.state)) {
        closingIntegration_ = true;
        if (PendingSave(snapshot.state) &&
            AfxMessageBox(L"미저장 검사 이력\n프로그램 종료", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            closingIntegration_ = false;
            closeRequested_ = false;
            integration_->Pause(false);
        } else {
            integration_->Close();
        }
        Refresh();
    }
    if (closingIntegration_ && integration_->Closed() && !historyOpen_.valid() && !historyRefresh_.valid()) {
        CDialogEx::OnCancel();
        return;
    }
    if (updated || displayChanged) Refresh();
}

void CWeldScopeAppDlg::ShowCurrentInspection(const IntegrationSnapshot& snapshot)
{
    ClearResult();
    fileName_.SetWindowText(std::filesystem::u8path(snapshot.record.sourcePath).c_str());
    if (snapshot.result) {
        ShowInspection(*snapshot.result);
        verdict_.SetWindowText(StoredDecision(snapshot.record.workState, snapshot.record.decision));
        shownInspection_ = snapshot.record.inspectionId;
    }
}
#endif

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
            if (!verdictValue_) dc->SetTextColor(RGB(83, 99, 120));
            else if (*verdictValue_ == BaselineDecision::Pass) dc->SetTextColor(RGB(0, 111, 85));
            else dc->SetTextColor(RGB(181, 42, 50));
        }
        const bool background = id == IDC_SECTION;
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
    if (pollTimer_) KillTimer(pollTimer_);
    CDialogEx::OnDestroy();
}
