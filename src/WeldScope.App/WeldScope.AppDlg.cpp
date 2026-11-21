#include "pch.h"
#include "WeldScope.App.h"
#include "WeldScope.AppDlg.h"
#include "afxdialogex.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <opencv2/imgproc.hpp>
#ifdef WELDSCOPE_S10
#include "LocalContrastInspection.h"
#include <cwchar>
#endif

namespace {
using namespace weldscope;
constexpr UINT_PTR inspectionTimerId = 1;

const wchar_t* VerdictText(const std::optional<InspectionOutcome>& verdict)
{
    if (!verdict) return L"판정 대기";
    switch (*verdict) {
    case InspectionOutcome::Pass: return L"정상";
    case InspectionOutcome::Fail: return L"불량";
    case InspectionOutcome::Review: return L"검토 필요";
    case InspectionOutcome::Error: return L"처리 오류";
    }
    return L"처리 오류";
}

#ifndef WELDSCOPE_S10
using State = InspectionJobState;

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
constexpr unsigned int folderPageSize = 100;
struct FolderResultFilter {
    const wchar_t* label;
    const char* decision;
    unsigned int BatchCounts::* count;
};

constexpr FolderResultFilter folderResultFilters[] = {
    {L"전체", "", nullptr},
    {L"정상", "PASS", &BatchCounts::passed},
    {L"불량", "FAIL", &BatchCounts::failed},
    {L"검토 필요", "REVIEW", &BatchCounts::review},
    {L"처리 오류", "ERROR", &BatchCounts::errors},
    {L"취소", "CANCELLED", &BatchCounts::cancelled},
};
static_assert(std::size(folderResultFilters) == IDC_FOLDER_CANCELLED - IDC_FOLDER_ALL + 1);

struct FolderDialogApartment {
    HRESULT result = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~FolderDialogApartment() { if (SUCCEEDED(result)) ::CoUninitialize(); }
};

unsigned short DevicePort()
{
    wchar_t text[6]{};
    const DWORD length = ::GetEnvironmentVariableW(L"WELDSCOPE_DEVICE_PORT", text, _countof(text));
    if (length == 0) return 33361;
    if (length >= _countof(text)) throw std::invalid_argument("Invalid device port");
    wchar_t* end = nullptr;
    const auto value = std::wcstoul(text, &end, 10);
    if (end != text + length || value > 65535) throw std::invalid_argument("Invalid device port");
    return static_cast<unsigned short>(value);
}

std::optional<InspectionOutcome> DisplayDecision(std::string_view decision) {
    if (decision == "PASS") return InspectionOutcome::Pass;
    if (decision == "FAIL") return InspectionOutcome::Fail;
    if (decision == "REVIEW") return InspectionOutcome::Review;
    if (decision == "ERROR") return InspectionOutcome::Error;
    return std::nullopt;
}

const wchar_t* StoredDecision(InspectionWorkState workState, std::string_view decision) {
    if (workState == InspectionWorkState::Cancelled) return L"중지";
    return VerdictText(DisplayDecision(decision));
}

void AppendAttempt(CString& details, std::string_view stage, std::optional<double> score, double durationMs) {
    if (!details.IsEmpty()) details += L"\n";
    const auto* stageName = stage == "PRECISE" ? L"정밀 분석" : stage == "POOLED_WIDTH" ? L"폭별 질감" :
        stage == "POOLED" ? L"통합 질감" : stage == "SPATIAL" ? L"공간 분석" :
        stage == "FAST" ? L"빠른 분석" : L"기준 분석";
    if (score) details.AppendFormat(L"%s %.4f  %.2f ms", stageName, *score, durationMs);
    else details.AppendFormat(L"%s 오류  %.2f ms", stageName, durationMs);
}

const wchar_t* InspectionReasonText(std::string_view reason)
{
    if (reason == "budget_exhausted") return L"시간 예산 초과";
    if (reason == "precise_uncertain") return L"정밀 점수 불확실";
    if (reason == "fast_threshold") return L"빠른 분석 기준";
    if (reason == "precise_threshold") return L"정밀 분석 기준";
    if (reason == "dark_pixel_ratio") return L"어두운 픽셀 비율";
    if (reason == "cancelled") return L"검사 중지";
    if (reason == "file_access") return L"파일 접근 실패";
    if (reason == "damaged_png") return L"PNG 손상";
    if (reason == "size_limit") return L"크기 한도 초과";
    if (reason == "unsupported_format") return L"지원 형식 오류";
    if (reason == "configuration") return L"설정 오류";
    return L"분석 오류";
}

void AppendReviewReason(CString& details, std::string_view reason)
{
    if (reason == "budget_exhausted" || reason == "precise_uncertain") {
        details += L"\n";
        details += InspectionReasonText(reason);
    }
}

unsigned int CountMatchingFolderResults(const BatchCounts& counts, unsigned int filterIndex)
{
    const auto count = folderResultFilters[filterIndex].count;
    return count ? counts.*count : counts.Total();
}

cv::Mat RebuildSavedCandidateOverlay(const cv::Mat& bgr, const InspectionRecord& record, const std::filesystem::path& baseline)
{
    const auto attempt = std::find_if(record.attempts.rbegin(), record.attempts.rend(),
        [](const AnalysisAttempt& stage) { return stage.score.has_value(); });
    if (attempt == record.attempts.rend()) return {};

    // An image-level model only produces a pixel mask when the precise stage ran.
    if (attempt->stage == "POOLED_WIDTH" || attempt->stage == "POOLED" || attempt->stage == "SPATIAL") return {};
    const bool isBaseline = record.algorithmVersion == inspection_version(InspectionMode::Baseline);
    auto path = baseline;
    if (!isBaseline) {
        struct SavedConfig { const char* version; const wchar_t* filename; };
        const SavedConfig configs[] = {
            {inspection_version(InspectionMode::Selective), L"selective.json"},
            {inspection_version(InspectionMode::PooledSmooth), pooledSmoothConfigFile},
            {"openzdm-spatial-48-v1", L"spatial-48.json"}, {"openzdm-pooled-400-v1", L"pooled-400.json"},
            {"openzdm-pooled-width-220-v1", L"pooled-width-220.json"}
        };
        const auto saved = std::find_if(std::begin(configs), std::end(configs),
            [&](const SavedConfig& entry) { return record.algorithmVersion == entry.version; });
        if (saved == std::end(configs)) throw std::runtime_error("The saved algorithm version is unavailable");
        path = baseline.parent_path() / saved->filename;
    }
    const auto config = read_bytes(path, config_file_limit);
    if (sha256(config) != record.configSha256) throw std::runtime_error("The saved settings are unavailable");

    cv::Mat mask;
    if (isBaseline) {
        mask = inspect_baseline(bgr, parse_baseline_config(read_json(config))).mask;
    } else {
        const bool precise = attempt->stage == "PRECISE";
        const auto settings = parse_inspection_parameters(read_json(config), InspectionMode::Selective);
        mask = inspect_local_contrast(bgr, precise ? LocalContrastMode::Precise : LocalContrastMode::Fast,
            settings.threshold, precise ? settings.preciseBand.failMin : settings.fastBand.failMin).mask;
    }
    if (!cv::countNonZero(mask)) return {};
    cv::Mat overlay;
    cv::cvtColor(make_candidate_overlay(bgr, mask), overlay, cv::COLOR_BGR2BGRA);
    return overlay;
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
    ON_NOTIFY(TCN_SELCHANGE, IDC_INSPECTION_TABS, OnInspectionTabChanged)
    ON_BN_CLICKED(IDC_FOLDER_PICKER, OnSelectFolder)
    ON_CBN_SELCHANGE(IDC_FOLDER_HISTORY, OnFolderHistory)
    ON_CONTROL_RANGE(BN_CLICKED, IDC_FOLDER_ALL, IDC_FOLDER_CANCELLED, OnFolderFilter)
    ON_BN_CLICKED(IDC_FOLDER_PREVIOUS, OnFolderPrevious)
    ON_BN_CLICKED(IDC_FOLDER_NEXT, OnFolderNext)
    ON_BN_CLICKED(IDC_FOLDER_OPEN, OnOpenFolderRow)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_FOLDER_RESULTS, OnFolderRowChanged)
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
    CString appTitle;
    GetWindowText(appTitle);
    label(title_, appTitle, IDC_TITLE);
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
    CreateFolderControls();
    try {
        cv::setNumThreads(1);
        const auto config = BaselineConfigPath();
        integration_ = std::make_unique<IntegratedInspection>(config, config.parent_path() / L"defect.png", DevicePort());
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

#ifdef WELDSCOPE_S10
void CWeldScopeAppDlg::CreateFolderControls()
{
    inspectionTabs_.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FOCUSONBUTTONDOWN,
        CRect(), this, IDC_INSPECTION_TABS);
    inspectionTabs_.InsertItem(0, L"파일 검사");
    inspectionTabs_.InsertItem(1, L"폴더 검사");

    constexpr DWORD button = WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON;
    folderPicker_.Create(L"폴더 선택", button, CRect(), this, IDC_FOLDER_PICKER);
    folderPath_.Create(L"폴더 선택 대기", WS_CHILD | SS_NOPREFIX | SS_PATHELLIPSIS,
        CRect(), this, IDC_FOLDER_PATH);
    folderProgress_.Create(L"저장 완료 0 / 전체 0", WS_CHILD | SS_NOPREFIX, CRect(), this, IDC_FOLDER_PROGRESS);
    folderStatus_.Create(L"폴더 선택 대기", WS_CHILD | SS_NOPREFIX, CRect(), this, IDC_FOLDER_STATUS);
    folderCurrent_.Create(L"", WS_CHILD | SS_NOPREFIX | SS_PATHELLIPSIS, CRect(), this, IDC_FOLDER_CURRENT);
    folderHistory_.Create(WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(), this, IDC_FOLDER_HISTORY);
    folderHistory_.AddString(L"새 폴더 검사");
    folderHistory_.SetCurSel(0);
    for (UINT index = 0; index < static_cast<UINT>(folderFilters_.size()); ++index) {
        folderFilters_[index].Create(folderResultFilters[index].label, WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON |
            BS_PUSHLIKE | (index == 0 ? WS_GROUP : 0), CRect(), this, IDC_FOLDER_ALL + index);
    }
    folderFilters_.front().SetCheck(BST_CHECKED);
    folderResults_.Create(WS_CHILD | WS_TABSTOP | WS_GROUP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        CRect(), this, IDC_FOLDER_RESULTS);
    folderResults_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    folderResults_.InsertColumn(0, L"상대 경로", LVCFMT_LEFT, Dip(260));
    folderResults_.InsertColumn(1, L"판정", LVCFMT_LEFT, Dip(100));
    folderResults_.InsertColumn(2, L"판정 사유", LVCFMT_LEFT, Dip(180));
    folderResults_.InsertColumn(3, L"분석 시간", LVCFMT_RIGHT, Dip(94));
    folderPrevious_.Create(L"이전", button, CRect(), this, IDC_FOLDER_PREVIOUS);
    folderNext_.Create(L"다음", button, CRect(), this, IDC_FOLDER_NEXT);
    folderPageNumber_.Create(L"1 / 1", WS_CHILD | SS_NOPREFIX | SS_CENTER, CRect(), this, IDC_FOLDER_PAGE);
    folderOpen_.Create(L"상세 보기", button, CRect(), this, IDC_FOLDER_OPEN);
}

void CWeldScopeAppDlg::LayoutFolder(int width, int contentHeight)
{
    const int margin = Dip(28), gap = Dip(20), panelWidth = width - 2 * margin;
    const int tableBottom = (std::max)(Dip(475), contentHeight - Dip(270));
    historyPanel_ = CRect(margin, Dip(279), width - margin, tableBottom);
    const int top = tableBottom + Dip(14), bottom = contentHeight - Dip(22);
    imagePanel_ = CRect(margin, top, width - margin - Dip(284) - gap, bottom);
    verdictPanel_ = CRect(imagePanel_.right + gap, top, width - margin, bottom);
    auto move = [this](CWnd& control, int x, int y, int itemWidth, int itemHeight) {
        control.MoveWindow(x, y - scrollOffset_, (std::max)(1, itemWidth), (std::max)(1, itemHeight));
    };
    move(folderPicker_, margin, Dip(118), Dip(112), Dip(34));
    move(folderPath_, margin + Dip(125), Dip(123), panelWidth - Dip(369), Dip(27));
    move(start_, width - margin - Dip(240), Dip(118), Dip(116), Dip(34));
    move(stop_, width - margin - Dip(116), Dip(118), Dip(116), Dip(34));
    move(folderHistory_, margin, Dip(166), Dip(224), Dip(220));
    move(folderProgress_, margin + Dip(237), Dip(170), Dip(248), Dip(25));
    move(folderStatus_, width - margin - Dip(186), Dip(170), Dip(186), Dip(25));
    move(folderCurrent_, margin, Dip(206), panelWidth, Dip(25));
    const int filterWidth = (panelWidth - Dip(40)) / static_cast<int>(folderFilters_.size());
    int filterX = margin;
    for (auto& filter : folderFilters_) {
        move(filter, filterX, Dip(238), filterWidth, Dip(34));
        filterX += filterWidth + Dip(8);
    }
    move(folderResults_, margin + Dip(10), Dip(289), panelWidth - Dip(20), tableBottom - Dip(331));
    const int sourceWidth = (std::max)(Dip(218), panelWidth - Dip(468));
    folderResults_.SetColumnWidth(0, sourceWidth);
    folderResults_.SetColumnWidth(1, Dip(110));
    folderResults_.SetColumnWidth(2, Dip(214));
    folderResults_.SetColumnWidth(3, Dip(100));
    move(folderPrevious_, margin + Dip(10), tableBottom - Dip(36), Dip(61), Dip(28));
    move(folderPageNumber_, margin + Dip(74), tableBottom - Dip(32), Dip(90), Dip(24));
    move(folderNext_, margin + Dip(167), tableBottom - Dip(36), Dip(61), Dip(28));
    move(folderOpen_, width - margin - Dip(118), tableBottom - Dip(36), Dip(108), Dip(28));
    move(imageTitle_, imagePanel_.left + gap, top + Dip(12), Dip(130), Dip(23));
    move(imageMeta_, imagePanel_.left + Dip(155), top + Dip(12), imagePanel_.Width() - Dip(176), Dip(23));
    move(image_, imagePanel_.left + gap, top + Dip(43), imagePanel_.Width() - 2 * gap, imagePanel_.Height() - Dip(78));
    move(fileName_, imagePanel_.left + gap, bottom - Dip(29), imagePanel_.Width() - 2 * gap, Dip(24));
    move(resultTitle_, verdictPanel_.left + gap, top + Dip(10), Dip(236), Dip(23));
    move(verdict_, verdictPanel_.left + gap, top + Dip(34), Dip(236), Dip(45));
    move(detail_, verdictPanel_.left + gap, top + Dip(84), Dip(236), Dip(63));
    move(candidate_, verdictPanel_.left + gap, bottom - Dip(35), Dip(236), Dip(28));
}
#endif

void CWeldScopeAppDlg::Layout()
{
    if (!history_.GetSafeHwnd() || isLayingOut_) return;
    isLayingOut_ = true;
    CRect bounds;
    GetClientRect(bounds);
    int minimumHeight = 640;
#ifdef WELDSCOPE_S10
    minimumHeight = folderMode_ ? 720 : 676;
#endif
    const int contentHeight = (std::max)(bounds.Height(), Dip(minimumHeight));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMax = contentHeight - 1;
    scroll.nPage = bounds.Height();
    scroll.nPos = scrollOffset_;
    SetScrollInfo(SB_VERT, &scroll, TRUE);
    scrollOffset_ = GetScrollPos(SB_VERT);
    GetClientRect(bounds);
    const int margin = Dip(28), gap = Dip(20), rightWidth = Dip(284);
    int fileControlsOffset = 0;
#ifdef WELDSCOPE_S10
    fileControlsOffset = Dip(36);
#endif
    const int top = Dip(152) + fileControlsOffset;
    const int panelBottom = (std::max)(top + Dip(288), contentHeight - Dip(264));
    imagePanel_ = CRect(margin, top, bounds.Width() - margin - gap - rightWidth, panelBottom);
    verdictPanel_ = CRect(imagePanel_.right + gap, top, bounds.Width() - margin, panelBottom);
    historyPanel_ = CRect(margin, panelBottom + gap, bounds.Width() - margin, contentHeight - Dip(44));
    auto move = [this](CWnd& control, int x, int y, int width, int height) {
        control.MoveWindow(x, y - scrollOffset_, (std::max)(1, width), (std::max)(1, height));
    };
    move(title_, margin, Dip(22), Dip(172), Dip(38));
#ifdef WELDSCOPE_S10
    move(connectionState_, margin + Dip(178), Dip(33), Dip(225), Dip(24));
    move(saveState_, bounds.Width() - margin - Dip(254), Dip(33), Dip(140), Dip(24));
    move(retrySave_, bounds.Width() - margin - Dip(110), Dip(24), Dip(110), Dip(36));
    move(inspectionTabs_, margin, Dip(79), bounds.Width() - 2 * margin, Dip(32));
    if (folderMode_) {
        LayoutFolder(bounds.Width(), contentHeight);
        isLayingOut_ = false;
        Invalidate(FALSE);
        return;
    }
#endif
    move(section_, margin, Dip(88) + fileControlsOffset, Dip(96), Dip(28));
    move(filePicker_, margin + Dip(110), Dip(83) + fileControlsOffset, Dip(160), Dip(36));
    move(start_, bounds.Width() - margin - Dip(240), Dip(83) + fileControlsOffset, Dip(116), Dip(36));
    move(stop_, bounds.Width() - margin - Dip(116), Dip(83) + fileControlsOffset, Dip(116), Dip(36));
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
    if (folderMode_) { StartFolder(); return; }
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
            verdictValue_ = inspected->outcome;
            image_.SetImages(inspected->original, inspected->overlay);
            CString metadata, details;
            metadata.Format(L"%d × %d px", inspected->original.cols, inspected->original.rows);
#ifdef WELDSCOPE_S10
            if (!inspected->attempts.empty() && inspected->attempts.front().stage != "BASELINE") {
                for (const auto& attempt : inspected->attempts) {
                    const auto duration = std::chrono::duration<double, std::milli>(attempt.elapsed).count();
                    AppendAttempt(details, attempt.stage, attempt.score, duration);
                }
                AppendReviewReason(details, inspected->reason);
            } else
#endif
            if (inspected->score) {
                details.Format(L"후보 점수 %.4f\n읽기 %.2f ms\n분석 %.2f ms",
                    *inspected->score, inspected->loadMs, inspected->analysisMs);
            }
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
    history_.SetItemText(row, 3, VerdictText(inspected.outcome));
    history_.SetItemText(row, 4, inspected.path.c_str());
}
#endif

#ifdef WELDSCOPE_S10
void CWeldScopeAppDlg::OnInspectionTabChanged(NMHDR*, LRESULT* result)
{
    *result = 0;
    const bool folder = inspectionTabs_.GetCurSel() == 1;
    if (folderMode_ == folder) return;
    folderMode_ = folder;
    ++folderSelectionRevision_;
    selectedFolderInspectionId_.clear();
    ClearResult();
    fileName_.SetWindowText(folderMode_ ? L"결과 선택" : selectedFile_.c_str());
    const int fileVisibility = folderMode_ ? SW_HIDE : SW_SHOW;
    for (CWnd* control : std::initializer_list<CWnd*>{&section_, &filePicker_, &statusTitle_, &status_,
        &historyTitle_, &historyNote_, &history_, &refreshHistory_, &openHistory_}) control->ShowWindow(fileVisibility);
    const int folderVisibility = folderMode_ ? SW_SHOW : SW_HIDE;
    for (CWnd* control : std::initializer_list<CWnd*>{&folderPicker_, &folderPath_, &folderProgress_,
        &folderStatus_, &folderCurrent_, &folderHistory_, &folderResults_, &folderPrevious_,
        &folderNext_, &folderPageNumber_, &folderOpen_}) control->ShowWindow(folderVisibility);
    for (auto& filter : folderFilters_) filter.ShowWindow(folderVisibility);
    SetScrollPos(SB_VERT, 0, TRUE);
    scrollOffset_ = 0;
    if (folderMode_) {
        RequestBatchHistory();
        folderPageRefreshPending_ = true;
    } else if (integration_) {
        const auto snapshot = integration_->Snapshot();
        if (snapshot.record.batchId.empty() && snapshot.result && view_ != InspectionView::FileSelection)
            ShowCurrentInspection(snapshot);
    }
    Layout();
    Refresh();
}

void CWeldScopeAppDlg::ResetFolderResults()
{
    ++folderSelectionRevision_;
    folderPageOffset_ = 0;
    folderFilterIndex_ = 0;
    folderFilters_.front().SetCheck(BST_CHECKED);
    for (std::size_t index = 1; index < folderFilters_.size(); ++index) folderFilters_[index].SetCheck(BST_UNCHECKED);
    folderCounts_ = {};
    folderRows_.clear();
    folderResults_.DeleteAllItems();
    selectedFolderInspectionId_.clear();
    if (folderMode_) {
        ClearResult();
        fileName_.SetWindowText(L"결과 선택");
    }
}

void CWeldScopeAppDlg::OnSelectFolder()
{
    if (closeRequested_ || !integration_ || folderScanTask_.valid() || !folderPicker_.IsWindowEnabled()) return;
    FolderDialogApartment apartment;
    if (FAILED(apartment.result)) {
        folderNotice_ = L"폴더 선택 실패";
        Refresh();
        return;
    }
    CFolderPickerDialog picker(folderRoot_.empty() ? nullptr : folderRoot_.c_str(),
        OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR, this, 0);
    if (picker.DoModal() != IDOK || closeRequested_) return;
    folderRoot_ = std::filesystem::path(picker.GetPathName().GetString());
    folderFiles_.clear();
    displayedBatch_.reset();
    folderHistory_.SetCurSel(0);
    folderNotice_.Empty();
    folderErrorPath_.clear();
    ResetFolderResults();
    try {
        folderScanTask_ = std::async(std::launch::async, [path = folderRoot_] { return FindInspectionFiles(path); });
    } catch (const std::exception&) { folderNotice_ = L"폴더 검색 실패"; }
    Refresh();
}

void CWeldScopeAppDlg::StartFolder()
{
    if (folderFiles_.empty() || folderScanTask_.valid()) return;
    try {
        if (integration_->StartFolder(folderRoot_, folderFiles_)) {
            folderFiles_.clear();
            folderNotice_.Empty();
            displayedBatch_.reset();
            ResetFolderResults();
        } else folderNotice_ = L"검사 시작 대기";
    } catch (const std::exception&) { folderNotice_ = L"검사 시작 실패"; }
    Refresh();
}

void CWeldScopeAppDlg::RequestBatchHistory()
{
    if (closeRequested_ || !integration_ || !integration_->Snapshot().batchReady) return;
    if (batchHistoryQuery_.valid()) { batchHistoryRefreshPending_ = true; return; }
    batchHistoryRefreshPending_ = false;
    try {
        batchHistoryQuery_ = std::async(std::launch::async, [] {
            return InspectionHistory(HistoryConnectionFromEnvironment()).RecentBatches();
        });
    } catch (const std::exception&) { folderNotice_ = L"폴더 이력 조회 실패"; }
}

void CWeldScopeAppDlg::ShowRecentFolderBatches(std::vector<BatchSummary> batches)
{
    recentFolderBatches_ = std::move(batches);
    folderHistory_.ResetContent();
    folderHistory_.AddString(L"새 폴더 검사");
    int selected = 0;
    for (std::size_t index = 0; index < recentFolderBatches_.size(); ++index) {
        const auto& batch = recentFolderBatches_[index];
        const auto path = std::filesystem::u8path(batch.rootPath);
        CString label;
        label.Format(L"%s (%s)", path.filename().empty() ? path.c_str() : path.filename().c_str(),
            WideAscii(batch.batchId.substr(0, 8)).c_str());
        const int item = folderHistory_.AddString(label);
        if (item < 0) break;
        if (displayedBatch_ && displayedBatch_->batchId == batch.batchId) {
            displayedBatch_->state = batch.state;
            selected = item;
        }
    }
    if (!displayedBatch_ && folderRoot_.empty() && !recentFolderBatches_.empty()) {
        folderHistory_.SetCurSel(1);
        OnFolderHistory();
    } else folderHistory_.SetCurSel(selected);
}

void CWeldScopeAppDlg::OnFolderHistory()
{
    const int selected = folderHistory_.GetCurSel();
    folderFiles_.clear();
    folderNotice_.Empty();
    folderErrorPath_.clear();
    displayedBatch_.reset();
    folderRoot_.clear();
    if (selected > 0 && static_cast<std::size_t>(selected) <= recentFolderBatches_.size()) {
        displayedBatch_ = recentFolderBatches_[static_cast<std::size_t>(selected - 1)];
        folderRoot_ = std::filesystem::u8path(displayedBatch_->rootPath);
    }
    ResetFolderResults();
    folderPageRefreshPending_ = displayedBatch_.has_value();
    Refresh();
}

void CWeldScopeAppDlg::OnFolderFilter(UINT id)
{
    folderFilterIndex_ = id - IDC_FOLDER_ALL;
    SelectFolderResultPage(0);
}

void CWeldScopeAppDlg::SelectFolderResultPage(unsigned int offset)
{
    folderPageOffset_ = offset;
    ++folderSelectionRevision_;
    folderPageRefreshPending_ = true;
    folderRows_.clear();
    folderResults_.DeleteAllItems();
    selectedFolderInspectionId_.clear();
    ClearResult();
    fileName_.SetWindowText(L"결과 선택");
    Refresh();
}

void CWeldScopeAppDlg::OnFolderPrevious()
{
    if (folderPageOffset_ == 0) return;
    SelectFolderResultPage(folderPageOffset_ - folderPageSize);
}

void CWeldScopeAppDlg::OnFolderNext()
{
    if (folderPageOffset_ + folderPageSize >= CountMatchingFolderResults(folderCounts_, folderFilterIndex_)) return;
    SelectFolderResultPage(folderPageOffset_ + folderPageSize);
}

void CWeldScopeAppDlg::RequestFolderPage()
{
    if (!displayedBatch_ || folderPageQuery_.valid() || !integration_->Snapshot().batchReady) return;
    folderPageRefreshPending_ = false;
    folderPageRequestRevision_ = folderSelectionRevision_;
    nextFolderPageRefresh_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    try {
        folderPageQuery_ = std::async(std::launch::async,
            [id = displayedBatch_->batchId, filter = folderResultFilters[folderFilterIndex_].decision, offset = folderPageOffset_] {
                return InspectionHistory(HistoryConnectionFromEnvironment()).ReadBatchPage(id, filter, offset);
            });
    } catch (const std::exception&) { folderNotice_ = L"폴더 결과 조회 실패"; }
}

void CWeldScopeAppDlg::ShowFolderPage(BatchPageView page)
{
    folderCounts_ = page.counts;
    folderNotice_.Empty();
    folderRows_ = std::move(page.rows);
    rebuildingFolderRows_ = true;
    folderResults_.SetRedraw(FALSE);
    folderResults_.DeleteAllItems();
    for (const auto& record : folderRows_) {
        const auto relative = std::filesystem::u8path(record.sourcePath).lexically_relative(folderRoot_);
        const int row = folderResults_.InsertItem(folderResults_.GetItemCount(), relative.c_str());
        if (row < 0) break;
        folderResults_.SetItemText(row, 1, StoredDecision(record.workState, record.decision));
        folderResults_.SetItemText(row, 2, InspectionReasonText(record.reason));
        CString duration;
        duration.Format(L"%.3f ms", record.durationMs);
        folderResults_.SetItemText(row, 3, duration);
        if (record.inspectionId == selectedFolderInspectionId_)
            folderResults_.SetItemState(row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    folderResults_.SetRedraw(TRUE);
    folderResults_.Invalidate(FALSE);
    rebuildingFolderRows_ = false;
}

void CWeldScopeAppDlg::OnOpenFolderRow()
{
    if (closeRequested_ || !folderMode_ || rebuildingFolderRows_) return;
    const int row = folderResults_.GetNextItem(-1, LVNI_SELECTED);
    if (row < 0 || static_cast<std::size_t>(row) >= folderRows_.size()) return;
    selectedFolderInspectionId_ = folderRows_[static_cast<std::size_t>(row)].inspectionId;
    if (!historyOpen_.valid()) RequestInspectionDetails(selectedFolderInspectionId_, true);
    Refresh();
}

void CWeldScopeAppDlg::OnFolderRowChanged(NMHDR* header, LRESULT* result)
{
    *result = 0;
    const auto& change = *reinterpret_cast<NMLISTVIEW*>(header);
    if ((change.uChanged & LVIF_STATE) && !(change.uOldState & LVIS_SELECTED) && (change.uNewState & LVIS_SELECTED))
        OnOpenFolderRow();
}

void CWeldScopeAppDlg::RefreshFolder(const IntegrationSnapshot& snapshot)
{
    const bool isBatchActive = snapshot.batch && snapshot.batch->active;
    const bool isStopping = isBatchActive && snapshot.batch->stopping;
    const bool isReady = snapshot.state == IntegrationState::Ready || snapshot.state == IntegrationState::Stored;
    const bool isScanning = folderScanTask_.valid();
    const auto totalFiles = displayedBatch_ ? displayedBatch_->totalFiles : static_cast<unsigned int>(folderFiles_.size());
    const auto savedFiles = displayedBatch_ ? folderCounts_.Total() : 0u;
    CString progress;
    progress.Format(L"저장 완료 %u / 전체 %u", savedFiles, totalFiles);
    folderProgress_.SetWindowText(progress);
    folderPath_.SetWindowText(folderRoot_.empty() ? L"폴더 선택 대기" : folderRoot_.c_str());

    CString status(L"폴더 선택 대기"), batchDetail;
    if (isReady && !snapshot.batchReady) status = L"폴더 검사 DB 준비 필요";
    else if (isScanning) status = L"폴더 검색 중";
    else if (isBatchActive) {
        status = L"검사 중";
        if (PendingSave(snapshot.state)) status = L"저장 재시도 대기";
        else if (isStopping) status = L"검사 중지 대기";
        if (displayedBatch_ && snapshot.record.batchId == displayedBatch_->batchId) {
            const auto relativePath = std::filesystem::u8path(snapshot.record.sourcePath).lexically_relative(folderRoot_);
            if (!relativePath.empty()) batchDetail.Format(L"현재 파일  %s", relativePath.c_str());
        }
    } else if (!folderNotice_.IsEmpty()) {
        status = folderNotice_;
        batchDetail = folderErrorPath_.c_str();
    } else if (displayedBatch_) {
        if (displayedBatch_->state == "completed") status = savedFiles == totalFiles ? L"검사 완료" : L"결과 집계 중";
        else if (displayedBatch_->state == "stopped") status = L"검사 중지";
        else if (snapshot.batch && snapshot.batch->batchId == displayedBatch_->batchId) status = L"결과 집계 중";
        else status = L"검사 중단";
        if (displayedBatch_->state != "completed" && savedFiles <= totalFiles)
            batchDetail.Format(L"취소 %u    미처리 %u", folderCounts_.cancelled, totalFiles - savedFiles);
    } else if (!folderFiles_.empty()) status = L"검사 대기";
    else if (!folderRoot_.empty()) status = L"PNG 파일 없음";
    folderStatus_.SetWindowText(status);
    folderCurrent_.SetWindowText(batchDetail);

    RefreshFolderResultControls(totalFiles);
    folderHistory_.EnableWindow(!closeRequested_ && snapshot.batchReady && !isBatchActive && !isScanning);
    folderPicker_.EnableWindow(!closeRequested_ && snapshot.batchReady && !isBatchActive && !isScanning &&
        !IntegrationBusy(snapshot.state) && !PendingSave(snapshot.state));
    start_.EnableWindow(!closeRequested_ && isReady && snapshot.batchReady && !isBatchActive &&
        snapshot.fileMode.has_value() && !folderFiles_.empty());
    stop_.EnableWindow(!closeRequested_ && isBatchActive && !isStopping);
}

void CWeldScopeAppDlg::RefreshFolderResultControls(unsigned int totalFiles)
{
    for (unsigned int index = 0; index < static_cast<unsigned int>(folderFilters_.size()); ++index) {
        const unsigned int count = index == 0 ? totalFiles : CountMatchingFolderResults(folderCounts_, index);
        CString text;
        text.Format(L"%s %u", folderResultFilters[index].label, count);
        folderFilters_[index].SetWindowText(text);
        folderFilters_[index].EnableWindow(!closeRequested_ && displayedBatch_.has_value());
    }
    const unsigned int matchingResults = CountMatchingFolderResults(folderCounts_, folderFilterIndex_);
    CString page;
    page.Format(L"%u / %u", folderPageOffset_ / folderPageSize + 1,
        matchingResults ? (matchingResults - 1) / folderPageSize + 1 : 1u);
    folderPageNumber_.SetWindowText(page);
    folderPrevious_.EnableWindow(!closeRequested_ && folderPageOffset_ > 0);
    folderNext_.EnableWindow(!closeRequested_ && folderPageOffset_ + folderPageSize < matchingResults);
    folderOpen_.EnableWindow(!closeRequested_ && folderResults_.GetNextItem(-1, LVNI_SELECTED) >= 0);
}

void CWeldScopeAppDlg::PollFolder(const IntegrationSnapshot& snapshot)
{
    PollFolderScan();
    TrackFolderBatch(snapshot);
    PollFolderQueries();
    if (!folderMode_ || closeRequested_) return;
    if (snapshot.batch && snapshot.batch->active && !PendingSave(snapshot.state) && displayedBatch_ &&
        snapshot.batch->batchId == displayedBatch_->batchId && snapshot.batch->savedFiles != folderCounts_.Total() &&
        std::chrono::steady_clock::now() >= nextFolderPageRefresh_)
        folderPageRefreshPending_ = true;
    if (folderPageRefreshPending_) RequestFolderPage();
    if (!selectedFolderInspectionId_.empty() && selectedFolderInspectionId_ != requestedFolderInspectionId_ &&
        !historyOpen_.valid())
        RequestInspectionDetails(selectedFolderInspectionId_, true);
}

void CWeldScopeAppDlg::PollFolderScan()
{
    using namespace std::chrono_literals;
    if (!folderScanTask_.valid() || folderScanTask_.wait_for(0ms) != std::future_status::ready) return;
    try {
        folderFiles_ = folderScanTask_.get();
    } catch (const std::filesystem::filesystem_error& error) {
        folderNotice_ = L"폴더 검색 실패";
        folderErrorPath_ = error.path1();
    } catch (const std::exception&) {
        folderNotice_ = L"폴더 검색 실패";
    }
    Refresh();
}

void CWeldScopeAppDlg::TrackFolderBatch(const IntegrationSnapshot& snapshot)
{
    const bool isBatchActive = snapshot.batch && snapshot.batch->active;
    const bool isNewBatch = snapshot.batch && lastObservedBatchId_ != snapshot.batch->batchId;
    if (isNewBatch) {
        lastObservedBatchId_ = snapshot.batch->batchId;
        displayedBatch_ = BatchSummary{lastObservedBatchId_, folderRoot_.u8string(), "running", snapshot.batch->totalFiles};
        ResetFolderResults();
        folderPageRefreshPending_ = true;
    }
    if (!isBatchActive && (isNewBatch || wasFolderBatchActive_)) {
        folderPageRefreshPending_ = true;
        RequestBatchHistory();
    }
    wasFolderBatchActive_ = isBatchActive;
}

void CWeldScopeAppDlg::PollFolderQueries()
{
    using namespace std::chrono_literals;
    if (batchHistoryQuery_.valid() && batchHistoryQuery_.wait_for(0ms) == std::future_status::ready) {
        try {
            auto batches = batchHistoryQuery_.get();
            if (!closeRequested_) ShowRecentFolderBatches(std::move(batches));
        } catch (const std::exception&) { folderNotice_ = L"폴더 이력 조회 실패"; }
        if (batchHistoryRefreshPending_) RequestBatchHistory();
        Refresh();
    }
    if (folderPageQuery_.valid() && folderPageQuery_.wait_for(0ms) == std::future_status::ready) {
        try {
            auto page = folderPageQuery_.get();
            if (!closeRequested_ && folderPageRequestRevision_ == folderSelectionRevision_) ShowFolderPage(std::move(page));
        } catch (const std::exception&) { folderNotice_ = L"폴더 결과 조회 실패"; }
        Refresh();
    }
}

void CWeldScopeAppDlg::RefreshIntegration()
{
    const auto snapshot = integration_ ? integration_->Snapshot() : IntegrationSnapshot{};
    const bool ready = snapshot.state == IntegrationState::Ready || snapshot.state == IntegrationState::Stored;
    const bool inspecting = snapshot.state == IntegrationState::Inspecting;
    const bool pending = PendingSave(snapshot.state);
    const bool folderActive = snapshot.batch && snapshot.batch->active;
    if (folderMode_) RefreshFolder(snapshot);
    else {
        start_.EnableWindow(!closeRequested_ && ready && !folderActive && snapshot.fileMode.has_value() && !selectedFile_.empty());
        stop_.EnableWindow(!closeRequested_ && inspecting && !folderActive);
    }
    if (snapshot.state != IntegrationState::Connecting)
        section_.SetWindowText(snapshot.fileMode ? L"연속 보간" : L"설정 오류");
    filePicker_.EnableWindow(!closeRequested_ && integration_ && !folderActive && !IntegrationBusy(snapshot.state) && !pending);
    retrySave_.EnableWindow(!closeRequested_ && (pending || snapshot.state == IntegrationState::Disconnected));
    retrySave_.SetWindowText(pending ? L"저장 재시도" : L"재연결");
    refreshHistory_.EnableWindow(!closeRequested_ && ready && !historyRefresh_.valid());
    openHistory_.EnableWindow(!closeRequested_ && ready && !historyRows_.empty() && !historyOpen_.valid());
    const wchar_t* task = L"대기";
    const wchar_t* storage = L"DB 연결 확인";
    switch (snapshot.state) {
    case IntegrationState::Disconnected: storage = L"DB 연결 끊김"; break;
    case IntegrationState::Ready: storage = L"DB 연결"; task = L"준비"; break;
    case IntegrationState::Inspecting:
        storage = L"DB 연결";
        if (snapshot.preciseRunning) task = L"정밀 분석";
        else if (snapshot.record.algorithmVersion == inspection_version(InspectionMode::Selective)) task = L"빠른 분석";
        else task = L"검사 중";
        break;
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
        if (const auto* result = std::get_if<InspectedFile>(&*snapshot.result);
            result && result->outcome == InspectionOutcome::Error) task = L"처리 오류";
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
    RequestInspectionDetails(historyRows_[static_cast<std::size_t>(row)].inspectionId, false);
    Refresh();
}

void CWeldScopeAppDlg::RequestInspectionDetails(const std::string& inspectionId, bool folder)
{
    isOpeningFolderResult_ = folder;
    if (folder) requestedFolderInspectionId_ = inspectionId;
    try {
        historyOpen_ = std::async(std::launch::async, [inspectionId, folder] {
            OpenedInspection opened;
            auto stored = InspectionHistory(HistoryConnectionFromEnvironment()).Get(inspectionId);
            if (!stored) throw std::runtime_error("Inspection history row unavailable");
            opened.record = std::move(*stored);
            const auto& record = opened.record.record;
            try {
                const auto bytes = read_bytes(std::filesystem::u8path(record.sourcePath), image_file_limit);
                if (record.sourceSha256.empty()) opened.sourceStatus = L"원본 확인 불가";
                else if (sha256(bytes) != record.sourceSha256) opened.sourceStatus = L"원본 변경";
                else {
                    const auto bgr = decode_png(bytes);
                    cv::cvtColor(bgr, opened.image, cv::COLOR_BGR2BGRA);
                    if (folder) {
                        try { opened.overlay = RebuildSavedCandidateOverlay(bgr, record, BaselineConfigPath()); }
                        catch (const std::exception&) { opened.sourceStatus = L"후보 표시 설정 확인"; }
                    }
                }
            } catch (const InspectionInputError& error) {
                opened.sourceStatus = error.reason() == FileInspectionFailure::FileAccess ? L"원본 누락" : L"원본 열기 실패";
            } catch (const std::exception&) { opened.sourceStatus = L"원본 열기 실패"; }
            return opened;
        });
    } catch (const std::exception&) {
        if (folder) detail_.SetWindowText(L"상세 조회 실패");
        else historyNote_.SetWindowText(L"이력 열기 실패");
    }
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

void CWeldScopeAppDlg::ShowOpenedInspection(const OpenedInspection& opened, bool folder)
{
    ClearResult();
    const auto& record = opened.record.record;
    if (!folder) {
        selectedFile_.clear();
        view_ = InspectionView::History;
        historyTask_ = record.workState == InspectionWorkState::Cancelled ? L"중지" :
            record.decision == "ERROR" ? L"처리 오류" : L"완료";
    }
    verdictValue_ = DisplayDecision(record.decision);
    verdict_.SetWindowText(StoredDecision(record.workState, record.decision));
    fileName_.SetWindowText(std::filesystem::u8path(record.sourcePath).c_str());
    image_.SetImages(opened.image, opened.overlay);
    imageMeta_.SetWindowText(opened.sourceStatus);
    CString details;
    if (folder || (!record.attempts.empty() && record.attempts.front().stage != "BASELINE")) {
        for (const auto& attempt : record.attempts)
            AppendAttempt(details, attempt.stage, attempt.score, attempt.durationMs);
        if (folder) {
            if (!details.IsEmpty()) details += L"\n";
            details += InspectionReasonText(record.reason);
        } else AppendReviewReason(details, record.reason);
    } else if (!record.attempts.empty() && record.attempts.front().score) {
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
    if (!folderMode_ && !closeRequested_ && view_ == InspectionView::PendingDeviceSave &&
        (snapshot.state == IntegrationState::Stored || shownInspection_ != snapshot.record.inspectionId)) {
        view_ = InspectionView::FileSelection;
        ClearResult();
        fileName_.SetWindowText(selectedFile_.c_str());
        displayChanged = true;
    }
    if (!folderMode_ && !closeRequested_ && view_ == InspectionView::FileSelection && fromDevice &&
        PendingSave(snapshot.state) && snapshot.result) {
        view_ = InspectionView::PendingDeviceSave;
        ShowCurrentInspection(snapshot);
        displayChanged = true;
    }
    if (updated) {
        integrationRevision_ = snapshot.revision;
        if (folderMode_ && snapshot.state == IntegrationState::Ready) RequestBatchHistory();
        if (!folderMode_ && (snapshot.state == IntegrationState::Ready || snapshot.state == IntegrationState::Stored))
            refreshHistoryPending_ = true;
        if (!folderMode_ && snapshot.record.batchId.empty() && !closeRequested_ &&
            !snapshot.record.inspectionId.empty() && shownInspection_ != snapshot.record.inspectionId) {
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
            if (!closeRequested_ && folderMode_ == isOpeningFolderResult_ &&
                (folderMode_ ? selectedFolderInspectionId_ == opened.record.record.inspectionId : snapshot.revision == historyOpenRevision_))
                ShowOpenedInspection(opened, folderMode_);
        } catch (const std::exception&) {
            if (folderMode_ && isOpeningFolderResult_ && selectedFolderInspectionId_ == requestedFolderInspectionId_)
                detail_.SetWindowText(L"상세 조회 실패");
            else if (!isOpeningFolderResult_) historyNote_.SetWindowText(L"이력 열기 실패");
        }
        Refresh();
    }
    if (refreshHistoryPending_ && !closeRequested_ && !historyRefresh_.valid() &&
        (snapshot.state == IntegrationState::Ready || snapshot.state == IntegrationState::Stored)) {
        refreshHistoryPending_ = false;
        OnRefreshHistory();
    }
    PollFolder(snapshot);
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
    if (closingIntegration_ && integration_->Closed() && !historyOpen_.valid() && !historyRefresh_.valid() &&
        !folderScanTask_.valid() && !batchHistoryQuery_.valid() && !folderPageQuery_.valid()) {
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
            else if (*verdictValue_ == InspectionOutcome::Pass) dc->SetTextColor(RGB(0, 111, 85));
            else if (*verdictValue_ == InspectionOutcome::Review) dc->SetTextColor(RGB(143, 78, 0));
            else dc->SetTextColor(RGB(181, 42, 50));
        }
        const bool background = id == IDC_SECTION || id == IDC_FOLDER_PATH || id == IDC_FOLDER_PROGRESS ||
            id == IDC_FOLDER_STATUS || id == IDC_FOLDER_CURRENT;
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
