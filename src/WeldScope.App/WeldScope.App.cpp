#include "pch.h"
#include "WeldScope.App.h"
#include "WeldScope.AppDlg.h"

CWeldScopeAppApp theApp;
BEGIN_MESSAGE_MAP(CWeldScopeAppApp, CWinApp)
END_MESSAGE_MAP()

CWeldScopeAppApp::CWeldScopeAppApp() {}
BOOL CWeldScopeAppApp::InitInstance()
{
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    CWinApp::InitInstance();
    AfxEnableControlContainer();
    CWeldScopeAppDlg dialog;
    m_pMainWnd = &dialog;
    modalResult_ = dialog.DoModal();
    m_pMainWnd = nullptr;
    return FALSE;
}

int CWeldScopeAppApp::ExitInstance()
{
    CWinApp::ExitInstance();
    // The modal loop's last message can be a timer, whose WPARAM is a timer ID.
    return modalResult_ == -1 ? 1 : 0;
}
