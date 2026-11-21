
#pragma once

#include "Resource.h"

#include <afxwin.h>


// MFC에서 앱의 시작과 종료를 받아 검사 대화상자를 한 번 실행한다.
class CWeldScopeAppApp : public CWinApp
{
public:
	// MFC가 등록해 사용할 앱 객체를 만든다. 창은 InitInstance에서 연다.
	CWeldScopeAppApp();

public:
	// 공용 컨트롤을 준비하고 검사 창을 모달로 연다. 창이 닫히면 FALSE를 반환해 앱을 끝낸다.
	virtual BOOL InitInstance();
	// 검사 창을 열지 못했으면 종료 코드 1, 정상적으로 닫혔으면 0을 반환한다.
	int ExitInstance() override;

	DECLARE_MESSAGE_MAP()

private:
	INT_PTR modalResult_ = -1;
};

// MFC가 앱의 수명을 제어하는 프로세스 전역 인스턴스.
extern CWeldScopeAppApp theApp;
