#pragma once
#include "FileInspectionJob.h"
#include "InspectionImageView.h"
#include "Resource.h"

#include <afxcmn.h>
#include <afxdialogex.h>
#include <optional>
#ifdef WELDSCOPE_S10
#include "InspectionDetails.h"
#include "IntegratedInspection.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>
#endif

// 창의 버튼, 목록, 영상 표시를 관리한다. 검사 실행과 저장은 검사 객체에 요청한다.
class CWeldScopeAppDlg : public CDialogEx {
public:
    // 선택적으로 부모 창을 지정한다. 컨트롤과 검사 객체는 창 초기화에서 준비한다.
    explicit CWeldScopeAppDlg(CWnd* parent = nullptr);
    // MFC가 모달 창을 만들 때 사용할 대화상자 리소스 ID.
    enum { IDD = IDD_WELDSCOPEAPP_DIALOG };
protected:
    // MFC가 창을 연 뒤 호출한다. 검사 모드에 맞는 컨트롤과 초기 상태를 준비한다.
    BOOL OnInitDialog() override;
    // 기본 키 처리를 맡긴 뒤 Tab으로 선택한 컨트롤이 보이도록 스크롤한다.
    BOOL PreTranslateMessage(MSG* message) override;
    // Enter 입력을 검사 시작에 연결해 대화상자를 닫지 않는다.
    void OnOK() override;
    // Esc나 닫기 요청에서 검사에 중지를 알리고, 백그라운드 작업 정리를 기다린다.
    void OnCancel() override;
    // 검사 영상, 판정, 이력 영역의 배경을 그린다.
    afx_msg void OnPaint();
    // 그리기에서 배경을 채우므로 별도 배경 지우기를 생략한다.
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    // 컨트롤이 사용할 배경 브러시와 판정 글자색을 전달한다.
    afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* wnd, UINT controlType);
    // 창 크기가 바뀌면 검사 컨트롤과 스크롤 범위를 다시 배치한다.
    afx_msg void OnSize(UINT type, int width, int height);
    // Windows에 검사 화면의 최소 창 크기를 알려 준다.
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info);
    // 세로 스크롤 바 요청으로 표시할 검사 화면의 위치를 바꾼다.
    afx_msg void OnVScroll(UINT code, UINT position, CScrollBar* bar);
    // 마우스 휠을 세로 이동에 연결한다.
    afx_msg BOOL OnMouseWheel(UINT flags, short delta, CPoint point);
    // 디스플레이 배율이 바뀌면 새 글꼴과 창 배치를 적용한다.
    afx_msg LRESULT OnDpiChanged(WPARAM dpi, LPARAM suggestedRect);
    // 파일 선택을 열고, 사용자가 확정한 경로를 검사 대상으로 보관한다.
    afx_msg void OnSelectFile();
    // 현재 탭의 단일 파일 또는 폴더 검사를 요청한다.
    afx_msg void OnStart();
    // 화면을 막지 않고 진행 중인 검사에 중지를 요청한다.
    afx_msg void OnStop();
    // 원본에 후보 표시를 덧그릴지 갱신한다. 검사를 다시 돌리지 않는다.
    afx_msg void OnToggleCandidate();
    // 검사 타이머에서 백그라운드 상태를 읽고 완료 결과를 회수한다.
    afx_msg void OnTimer(UINT_PTR id);
    // 제목 표시줄의 닫기를 취소 및 종료 처리와 같은 경로로 보낸다.
    afx_msg void OnClose();
    // 창이 파괴되기 전에 검사 상태를 읽던 타이머를 정리한다.
    afx_msg void OnDestroy();
    DECLARE_MESSAGE_MAP()
private:
    int Dip(int value) const;
    void Layout();
    void SetFonts();
    void Refresh();
    void ClearResult();
    void ShowInspection(const weldscope::FileInspectionResult& result);
#ifdef WELDSCOPE_S10
    enum class InspectionView { Live, FileSelection, History, PendingDeviceSave };
    // 저장 재시도 또는 재연결 버튼에서 현재 상태에 맞는 복구를 요청한다.
    void OnRetrySave();
    // 새로 고침 버튼에서 최근 단일 검사 목록의 조회를 요청한다.
    void OnRefreshHistory();
    // 선택한 검사 행의 저장 기록과 원본을 백그라운드에서 읽도록 요청한다.
    void OnOpenHistory();
    void RequestInspectionDetails(const std::string& inspectionId, weldscope::InspectionDetailSource source);
    void PollIntegration();
    void RefreshIntegration();
    void ShowHistory(std::vector<weldscope::InspectionSummary> rows);
    void ShowOpenedInspection(const weldscope::OpenedInspection& opened, weldscope::InspectionDetailSource source);
    void ShowCurrentInspection(const weldscope::IntegrationSnapshot& snapshot);
    void CreateFolderControls();
    void LayoutFolder(int width, int contentHeight);
    // 파일과 폴더 탭을 바꾸면 표시할 제어와 결과를 갱신한다.
    void OnInspectionTabChanged(NMHDR*, LRESULT* result);
    // 선택한 폴더에서 검사할 PNG의 목록을 백그라운드로 검색한다.
    void OnSelectFolder();
    // 이전 폴더 검사나 새 검사를 선택할 때 목록과 상세 보기를 해당 선택으로 전환한다.
    void OnFolderHistory();
    // 선택한 판정 필터를 적용하고 첫 페이지의 조회를 요청한다.
    void OnFolderFilter(UINT id);
    // 이전 페이지나 다음 페이지가 있을 때만 해당 목록을 요청한다.
    void OnFolderPrevious();
    void OnFolderNext();
    // 목록에서 선택한 폴더 검사 행의 상세 조회를 요청한다.
    void OnOpenFolderRow();
    // 사용자가 새 행을 선택하면 해당 검사 상세를 열도록 요청한다.
    void OnFolderRowChanged(NMHDR* header, LRESULT* result);
    void SelectFolderResultPage(unsigned int offset);
    void ResetFolderResults();
    void StartFolder();
    void RefreshFolder(const weldscope::IntegrationSnapshot& snapshot);
    void RefreshFolderResultControls(unsigned int totalFiles);
    void PollFolder(const weldscope::IntegrationSnapshot& snapshot);
    void PollFolderScan();
    void PollFolderQueries();
    void TrackFolderBatch(const weldscope::IntegrationSnapshot& snapshot);
    void RequestBatchHistory();
    void RequestFolderPage();
    void ShowRecentFolderBatches(std::vector<weldscope::BatchSummary> batches);
    void ShowFolderPage(weldscope::BatchPageView page);
    std::unique_ptr<weldscope::IntegratedInspection> integration_;
    std::future<std::vector<weldscope::InspectionSummary>> historyRefresh_;
    std::future<weldscope::OpenedInspection> historyOpen_;
    weldscope::InspectionDetailSource historyOpenSource_ = weldscope::InspectionDetailSource::SingleFileHistory;
    std::vector<weldscope::InspectionSummary> historyRows_;
    CStatic saveState_, connectionState_;
    CButton retrySave_, refreshHistory_, openHistory_;
    std::filesystem::path selectedFile_;
    std::uint64_t integrationRevision_ = 0;
    std::uint64_t historyOpenRevision_ = 0;
    std::string shownInspection_;
    InspectionView view_ = InspectionView::Live;
    const wchar_t* historyTask_ = L"완료";
    bool closeRequested_ = false, closingIntegration_ = false, refreshHistoryPending_ = false;
    CTabCtrl inspectionTabs_;
    CButton folderPicker_, folderPrevious_, folderNext_, folderOpen_;
    CStatic folderPath_, folderProgress_, folderStatus_, folderPageNumber_, folderCurrent_;
    CComboBox folderHistory_;
    CListCtrl folderResults_;
    std::array<CButton, IDC_FOLDER_CANCELLED - IDC_FOLDER_ALL + 1> folderFilters_;
    std::filesystem::path folderRoot_;
    std::vector<std::filesystem::path> folderFiles_;
    std::future<std::vector<std::filesystem::path>> folderScanTask_;
    std::future<std::vector<weldscope::BatchSummary>> batchHistoryQuery_;
    std::future<weldscope::BatchPageView> folderPageQuery_;
    std::vector<weldscope::BatchSummary> recentFolderBatches_;
    std::vector<weldscope::BatchRow> folderRows_;
    std::optional<weldscope::BatchSummary> displayedBatch_;
    weldscope::BatchCounts folderCounts_;
    CString folderNotice_;
    std::filesystem::path folderErrorPath_;
    std::string lastObservedBatchId_, selectedFolderInspectionId_, requestedFolderInspectionId_;
    std::chrono::steady_clock::time_point nextFolderPageRefresh_;
    unsigned int folderFilterIndex_ = 0, folderPageOffset_ = 0, folderSelectionRevision_ = 0, folderPageRequestRevision_ = 0;
    bool folderMode_ = false, folderPageRefreshPending_ = false, rebuildingFolderRows_ = false;
    bool wasFolderBatchActive_ = false, batchHistoryRefreshPending_ = false;
#else
    void AddHistory(const weldscope::InspectedFile& inspected);
    weldscope::FileInspectionJob job_;
#endif
    CRect imagePanel_, verdictPanel_, historyPanel_;
    CFont bodyFont_, titleFont_, verdictFont_, labelFont_;
    CBrush whiteBrush_{RGB(255, 255, 255)}, backgroundBrush_{RGB(243, 246, 250)};
    CStatic title_, section_, imageTitle_, imageMeta_, fileName_, resultTitle_, verdict_,
        statusTitle_, status_, detail_, historyTitle_, historyNote_;
    CButton filePicker_, start_, stop_, candidate_;
    CListCtrl history_;
    InspectionImageView image_;
    std::optional<weldscope::InspectionOutcome> verdictValue_;
    UINT dpi_ = 96;
    UINT_PTR pollTimer_ = 0;
    int scrollOffset_ = 0;
    bool isLayingOut_ = false;
};
