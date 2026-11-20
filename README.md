# WeldScope

WeldScope는 공개 용접 외관 PNG를 분석하는 C++ 검사 프로그램입니다. Windows MFC에서 원본과 후보 오버레이를 확인하고, 파일 검사와 루프백 TCP 장비 요청을 MySQL 이력으로 저장합니다.

![WeldScope의 실제 MFC 두 단계 분석 화면](docs/steps/S11-evidence/gui/04-frozen-two-stage.png)

## 실행과 재현

- **검수용 ZIP** — Windows 11에서 x64 패키지를 풀고 VC++ 런타임과 전용 MySQL을 연결합니다. 파일 선택, 정밀 분석, TCP 재전송과 이력 조회를 시연할 수 있습니다. [실행 안내](docs/steps/S12-run-package.md) · [패키지 생성과 검증](docs/steps/S12-delivery-reproducibility.md)
- **소스 빌드** — Visual Studio에서 동일한 MFC 앱과 CMake 검사 CLI를 빌드합니다. [S11 빌드와 검사 명령](docs/steps/S11-algorithm-integration-evaluation.md#실행과-검수)
- **평가 자료** — OPENZDM은 공식 분할을 사용합니다. 원본 취득 절차, 공용 검증 규칙, 설정 및 동결 기록에서 평가를 재현할 수 있습니다. [데이터 취득](docs/steps/S02-data-preparation.md) · [평가 기준](docs/adr/0011-algorithm-integration-evaluation.md) · [의존성과 영상 출처](docs/steps/S12-third-party-notices.md)

## 구현과 결과

- **C++ 분석과 정책** — 전역 밝기 기준선, 국소 대비 빠른 분석, 다중 스케일 정밀 분석을 구현했습니다. 불확실한 빠른 점수에 정밀 분석을 최대 한 번 적용합니다. [알고리즘 비교와 실패 사례](docs/steps/S07-improved-inspection.md) · [정밀 분석 정책](docs/steps/S08-selective-inspection-policy.md)
- **앱과 통합** — MFC 화면과 작업 수명, Winsock 기반 루프백 장비 프로토콜, MySQL C API 기반 이력 저장·재조회·재전송을 구현했습니다. 영상 연산과 PNG 처리에는 OpenCV를 사용합니다. [파일 검사](docs/steps/S09-file-inspection-integration.md) · [TCP와 MySQL](docs/steps/S10-record-device-integration.md)
- **S11 비교** — 최초 동결 주 시험에서 선택적 분석은 정상 오판을 줄였고, KO 자동 통과는 두 반복에서 754건·752건으로 기준 경로의 750건보다 많았습니다. 주 시험은 유사한 교차 분할 그룹을 제외했으며 부품 단위 독립성은 미확인입니다. 시간은 Windows 11 ARM QEMU VM의 x64 Release에서 측정했습니다. [네 경로와 두 시험 집합의 결과](docs/steps/S11-algorithm-integration-evaluation.md#시험-결과)
