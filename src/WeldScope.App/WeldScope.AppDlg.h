#pragma once
#include "FileInspectionJob.h"
#include "InspectionImageView.h"
#ifdef WELDSCOPE_S10
#include "IntegratedInspection.h"
#include <array>
#endif

class CWeldScopeAppDlg : public CDialogEx {
public:
    explicit CWeldScopeAppDlg(CWnd* parent = nullptr);
    enum { IDD = IDD_WELDSCOPEAPP_DIALOG };
protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* message) override;
    void OnOK() override;
    void OnCancel() override;
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* wnd, UINT controlType);
    afx_msg void OnSize(UINT type, int width, int height);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info);
    afx_msg void OnVScroll(UINT code, UINT position, CScrollBar* bar);
    afx_msg BOOL OnMouseWheel(UINT flags, short delta, CPoint point);
    afx_msg LRESULT OnDpiChanged(WPARAM dpi, LPARAM suggestedRect);
    afx_msg void OnSelectFile();
    afx_msg void OnStart();
    afx_msg void OnStop();
    afx_msg void OnToggleCandidate();
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnClose();
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
    struct OpenedInspection {
        weldscope::StoredInspection record;
        cv::Mat image, overlay;
        CString sourceStatus;
    };
    void OnRetrySave();
    void OnRefreshHistory();
    void OnOpenHistory();
    void RequestInspectionDetails(const std::string& inspectionId, bool folder);
    void PollIntegration();
    void RefreshIntegration();
    void ShowHistory(std::vector<weldscope::InspectionSummary> rows);
    void ShowOpenedInspection(const OpenedInspection& opened, bool folder);
    void ShowCurrentInspection(const weldscope::IntegrationSnapshot& snapshot);
    void CreateFolderControls();
    void LayoutFolder(int width, int contentHeight);
    void OnInspectionTabChanged(NMHDR*, LRESULT* result);
    void OnSelectFolder();
    void OnFolderHistory();
    void OnFolderFilter(UINT id);
    void OnFolderPrevious();
    void OnFolderNext();
    void OnOpenFolderRow();
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
    std::future<OpenedInspection> historyOpen_;
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
    bool isOpeningFolderResult_ = false, wasFolderBatchActive_ = false, batchHistoryRefreshPending_ = false;
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
