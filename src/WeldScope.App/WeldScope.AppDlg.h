#pragma once
#include "SampleSession.h"
#include "SampleImageView.h"

class CWeldScopeAppDlg : public CDialogEx {
public:
    explicit CWeldScopeAppDlg(CWnd* parent = nullptr);
    enum { IDD = IDD_WELDSCOPEAPP_DIALOG };
protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* message) override;
    void OnOK() override;
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* wnd, UINT controlType);
    afx_msg void OnSize(UINT type, int width, int height);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info);
    afx_msg void OnVScroll(UINT code, UINT position, CScrollBar* bar);
    afx_msg BOOL OnMouseWheel(UINT flags, short delta, CPoint point);
    afx_msg LRESULT OnDpiChanged(WPARAM dpi, LPARAM suggestedRect);
    afx_msg void OnSelectSample();
    afx_msg void OnStart();
    afx_msg void OnStop();
    afx_msg void OnToggleCandidate();
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnDestroy();
    DECLARE_MESSAGE_MAP()
private:
    int Dip(int value) const;
    void Layout();
    void SetFonts();
    void Refresh();
    void AddHistory();
    CRect imagePanel_, verdictPanel_, historyPanel_;
    CFont bodyFont_, titleFont_, verdictFont_, labelFont_;
    CBrush whiteBrush_{RGB(255, 255, 255)}, backgroundBrush_{RGB(243, 246, 250)};
    CStatic title_, badge_, section_, imageTitle_, imageMeta_, fileName_, resultTitle_, verdict_,
        statusTitle_, status_, detail_, historyTitle_, historyNote_, footer_;
    CComboBox samplePicker_;
    CButton start_, stop_, candidate_;
    CListCtrl history_;
    SampleImageView image_;
    weldscope::SampleSession session_;
    int selectedSample_ = -1;
    UINT dpi_ = 96;
    UINT_PTR completionTimer_ = 0;
    UINT_PTR nextTimerId_ = 1;
    int scrollOffset_ = 0;
    bool isLayingOut_ = false;
};
