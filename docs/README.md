# WeldScope 문서 안내

## 단일 기준 문서

| 주제 | 기준 문서 |
|---|---|
| 제품 목적, 기능, 평가 범위 | [PRD](https://chatgpt.com/space/page_b35b9c74caac81918226ccdc9e38769e) |
| 전체 진행 순서, 의존성, 검수 상태 | [로드맵](ROADMAP.md) |
| OOP, 코드 품질, 아키텍처 | [엔지니어링 원칙](standards/engineering.md) |
| 디자인 스킬, 화면과 UI 문구 | [UI UX 원칙](standards/ui-ux.md) |
| 문서 수명, 삭제, 완료 점검 | [문서 관리](standards/maintenance.md) |
| 중요한 아키텍처 결정 | [ADR 목록과 절차](adr/README.md) |
| S04 기준 알고리즘의 계산식, 선택 근거, 점수 해석 | [전역 어두운 픽셀 비율](algorithms/global-dark-ratio.md) |

현재 폴더에는 Git 저장소와 MFC 프로젝트가 있다. S01은 사용자 완료 확인을 받았으며 당시 OpenCV 이미지 뷰어의 상세 실행 증거를 재검증한 상태는 아니다. [S02 데이터 준비](steps/S02-data-preparation.md)는 공식 데이터 전체 점검과 접촉표 생성을 수행했고 사용자 검수 대기다. [S03 검사 콘솔 시제품](steps/S03-screen-design.md)은 승인된 계획에 따라 구현했으며, 왜·어떻게·무엇을 중심의 설명과 실제 MFC 화면, 실행·검증 절차를 제공한다. 단계 상태는 [로드맵](ROADMAP.md), 기존 개발 환경 기록은 [S01 기록](steps/S01-development-environment.md)을 따른다. 전체 PRD의 세부 요구사항은 사용자 검수 대상이다.

[S04 C++ 기준 검사](steps/S04-baseline-inspection.md)는 사용자 검수 후 main에 병합됐다. 실제 네 단계 그림, train 43,169장의 두 번 실행과 재현 비교, 한 명령 시연과 기준의 실패 사례를 제공한다. S02의 독립 평가 사용 정책은 보류 상태다. [S05 검사 이력](steps/S05-storage-history.md)에 MySQL 저장·조회·재전송의 시연 절차와 검증 결과가 있다.

[S06 가상 장비 통신](steps/S06-device-communication.md)에 TCP 요청·샘플 응답, 타임아웃, 재연결과 재전송의 시연 절차와 검증 결과가 있다.

[S07 개선 알고리즘](steps/S07-improved-inspection.md)에 같은 train 영상의 기준·빠른·정밀 분석 비교, 실패 사례와 원본 크기별 비용이 있다.

[S08 선택적 정밀 분석](steps/S08-selective-inspection-policy.md)에 정밀 호출 횟수, 시간 예산과 최종 판정의 샘플 시연이 있다.

[S09 파일 검사 통합](steps/S09-file-inspection-integration.md)에 실제 PNG의 MFC 검사, 후보 표시, 취소와 오류 복구의 시연 절차가 있다.

[S10 기록과 장비 통합](steps/S10-record-device-integration.md)에 파일·TCP 입력의 MySQL 저장, 재조회와 장애 복구 검증을 기록한다.

이 폴더의 진행 단계는 ROADMAP.md가 기준이다. PRD에는 단계 구분만 요약하고 상태를 중복 기록하지 않는다. 소프트웨어 버전, 실행 명령, 빌드 옵션은 실제 구성 파일과 각 단계의 검증 결과를 기준으로 관리한다.

Agent.md와 AGENTS.md를 중복 유지하지 않는다. 에이전트 진입 문서는 프로젝트 루트의 AGENTS.md 하나다.
