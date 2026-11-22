# WeldScope

WeldScope는 공개 용접 외관 PNG를 분석하는 C++ 검사 프로그램입니다. Windows MFC에서 파일이나 폴더를 선택해 검사하고, 원본 이미지와 MySQL 이력을 확인할 수 있습니다. 루프백 TCP 장비 요청도 같은 검사 경로를 사용합니다.

![LightGBM 단일 검사 화면. 원본 이미지 옆에 검토 필요, 판정 점수, 분석 시간, 판정 기준 사이 사유가 표시됨](docs/algorithms/evidence/lightgbm-v2/review-screen.png)

## 실행과 재현

- **폴더 검사 ZIP** — Windows 11에서 x64 패키지를 풀고 런처를 더블클릭합니다. 폴더를 한 번 선택해 검사하고 진행률과 판정별 이력을 확인할 수 있습니다. [실행 안내](docs/steps/S13-run-package.md) · [전체 GUI E2E](docs/steps/S13-folder-inspection-monitoring.md)
- **소스 빌드** — Visual Studio에서 `weld-scope.slnx`를 열고 x64 Release로 빌드합니다. x64 MFC와 같은 구성의 OpenCV가 필요합니다. 빌드가 기본 모델과 설정을 실행 파일 옆에 복사합니다. [현재 앱 빌드·실행·ZIP 생성](docs/steps/S13-run-package.md#빌드와-zip-생성)
- **평가 자료** — OPENZDM은 공식 분할을 사용합니다. 원본 취득 절차, 공용 검증 규칙, 설정 및 동결 기록에서 평가를 재현할 수 있습니다. [데이터 취득](docs/steps/S02-data-preparation.md) · [평가 기준](docs/adr/0011-algorithm-integration-evaluation.md) · [의존성과 영상 출처](docs/steps/S12-third-party-notices.md)

## 구현과 결과

- **LightGBM 검사** — 위치별 밝기·무늬에서 점수를 한 번 계산합니다. 점수가 정상선과 불량선 사이이면 작업자가 원본을 확인합니다. [버전 2 검수](docs/algorithms/openzdm-false-pass-eda.md#버전-2-검수)
- **앱과 통합** — MFC 화면과 작업 수명, Winsock 기반 루프백 장비 프로토콜, MySQL C API 기반 이력 저장·재조회·재전송을 구현했습니다. 영상 연산과 PNG 처리에는 OpenCV를 사용합니다. [파일 검사](docs/steps/S09-file-inspection-integration.md) · [TCP와 MySQL](docs/steps/S10-record-device-integration.md)
