#pragma once
#include "FileInspectionJob.h"
#include "InspectionImageView.h"

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
    void AddHistory(const weldscope::InspectedFile& inspected);
    CRect imagePanel_, verdictPanel_, historyPanel_;
    CFont bodyFont_, titleFont_, verdictFont_, labelFont_;
    CBrush whiteBrush_{RGB(255, 255, 255)}, backgroundBrush_{RGB(243, 246, 250)};
    CStatic title_, section_, imageTitle_, imageMeta_, fileName_, resultTitle_, verdict_,
        statusTitle_, status_, detail_, historyTitle_, historyNote_;
    CButton filePicker_, start_, stop_, candidate_;
    CListCtrl history_;
    InspectionImageView image_;
    weldscope::FileInspectionJob job_;
    std::optional<weldscope::BaselineDecision> verdictValue_;
    UINT dpi_ = 96;
    UINT_PTR pollTimer_ = 0;
    int scrollOffset_ = 0;
    bool isLayingOut_ = false;
};
