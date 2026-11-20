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
    dialog.DoModal();
    return FALSE;
}
