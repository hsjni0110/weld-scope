# S13 폴더 일괄 검사와 모니터링 실행 계획

**상태**　구현 및 전체 GUI E2E 완료 · 사용자 결과 검수 대기

**기준**　S12 커밋 `063251a`

[PRD](https://chatgpt.com/space/page_b35b9c74caac81918226ccdc9e38769e) · [로드맵](../ROADMAP.md) · [기존 MFC·DB·TCP 계약](../adr/0010-record-device-integration.md)

## 사용 흐름

Windows에서 WeldScope를 열고 **폴더 검사 → 폴더 선택 → 검사 시작**을 누른다. 하위 폴더까지 검색한 PNG 수를 시작 전에 확인한다. 검사 중에는 진행 상황을 보고, 완료 후에는 **정상**, **불량**, **검토 필요**, **처리 오류**를 눌러 해당 결과만 확인한다. 결과를 선택하면 기존 원본·후보 오버레이, 단계별 점수·시간, 최종 판정 사유를 표시한다. 앱을 다시 열어 저장된 폴더 검사 결과를 조회할 수 있다.

검수 환경에서는 더블클릭으로 앱과 전용 MySQL을 연다. 런타임과 MySQL 경로는 처음 한 번 준비한다. 이후 폴더 선택, 검사, 저장, 조회는 앱에서 수행한다. [패키지 실행 안내](S13-run-package.md)

## 화면 구성

| 구성 | 검토 결과 |
|---|---|
| **기존 창의 폴더 검사 탭** | 목록·집계·상세를 함께 볼 수 있다. 단일 검사 화면의 원본·오버레이 표시를 재사용한다. |
| 별도 모니터링 창 | 두 화면을 동시에 볼 수 있다. 창 전환과 실행 선택 상태를 함께 유지해야 한다. |

Visualize의 직접 라벨, 주요 동작 우선, 필요한 정보만 표시하는 원칙을 네이티브 MFC 화면에 적용한다. 버튼과 필터는 키보드로 접근할 수 있게 하고, 판정은 색과 함께 항상 텍스트로 표시한다.

```text
파일 검사    폴더 검사

[폴더 선택]   선택한 경로                   [검사 시작] [검사 중지]
저장 완료 n / 전체 N                    현재 파일  val/OK/image_123.png

전체 N    정상 n    불량 n    검토 필요 n    처리 오류 n

상대 경로                     판정         판정 사유        처리시간
─────────────────────────────────────────────────────────────────
선택된 이미지의 원본과 후보 오버레이       단계별 점수와 시간
```

폴더 선택 전, 검색 중, 실패 상태를 각각 표시한다. 중지 시에는 취소와 미처리 수를 표시한다. 정상·불량은 앱의 판정이다. OPENZDM의 OK·KO 정답 라벨은 검수 자료에서 따로 비교한다. 실제 MFC 화면으로 작은 창, 키보드, 긴 경로, DPI 변경 상태를 확인한다.

## 구현 순서와 수락 기준

| 작업 | 구현 범위 | 수락 기준 |
|---|---|---|
| **1. 폴더 입력과 검사 조정** | `src/WeldScope.App`에 Windows 기본 폴더 선택기를 연결한다. C++에서 검사 대상 상대 경로를 시작 전에 확정하고 정렬한다. `IntegratedInspection`의 단일 실행 슬롯과 `FileInspectionJob`·`AnalysisPipeline`으로 한 장씩 분석·저장한다. | 하위 폴더와 대소문자가 다른 PNG 확장자를 처리한다. 같은 이름의 파일은 상대 경로로 구분한다. 빈 폴더·탐색 실패를 표시하고 디렉터리 링크는 따라가지 않는다. 시작 후 추가한 파일은 다음 검사에 반영한다. 검사 중 창 조작과 중지가 가능하고 새 TCP 요청은 기존 `BUSY` 계약을 따른다. |
| **2. 저장과 재조회** | `src/WeldScope.Storage`와 `db`에 검사 묶음과 검사 행의 선택적 묶음 ID·순서를 추가한다. 묶음에 입력 루트, 설정 해시, 대상 수, 실행 상태를 기록한다. 기존 트랜잭션과 묶음별 고유 순서를 사용한다. | 기존 파일·TCP 이력을 읽을 수 있다. 저장 실패 시 새 파일 시작을 멈추고 같은 검사 ID로 저장만 재시도한다. 저장 확정 행만 집계한다. 강제 종료 후 부분 결과는 중단 상태로 조회하고 저장 수와 미처리 수를 확인한다. |
| **3. 화면과 실행 진입점** | 폴더 탭에서 진행·집계·판정 필터를 표시한다. 집계 조회는 UI 스레드 밖에서 수행한다. 결과는 페이지당 최대 100행을 불러오고 기존 이미지 상세를 사용한다. | 필터 숫자와 목록이 일치한다. 진행 중 필터 전환과 중지가 가능하다. 앱을 다시 열어 같은 검사 묶음을 조회한다. 실습자가 명령 입력 없이 런처를 열어 검사 결과까지 확인한다. |
| **4. E2E 검증** | 기존 Windows UI 검사 도구를 재사용하는 `tools/verify_folder_e2e.py`를 작성한다. 아래 작은 검수와 전체 데이터 검수를 수행한다. | 행 누락·중복 0건, DB·화면 집계 일치, 실제 화면 캡처와 재현 절차를 확인한다. |

묶음 헤더와 멤버십을 별도 테이블로 추가해 기존 검사 행을 보존한다. 멤버십, 검사 행과 분석 시도는 동일 트랜잭션에 저장한다. 기존 MySQL 계정의 읽기·삽입 권한에 `inspection_batches.state`의 수정 권한을 추가한다. [ADR 0012](../adr/0012-folder-inspection-monitoring.md)

## 시간 예산과 판정

현재 S11의 **132µs** 설정으로 실습한 네 건은 모두 시간 초과로 보류됐다. 새 폴더 검사 경로에서 실제 분석 시간을 측정하고, 폴더 검사의 설정은 시작할 때 확정한다. 실습 환경에 다른 예산이 필요하면 **검증 세트**에서 불량 자동 통과 및 보류율을 확인한 로컬 설정을 제안한다. 사용한 설정 이름·해시와 측정 조건을 기록한다. S11 동결 시험과 등록 TCP 검사는 각각 기록된 설정을 사용한다.

## E2E 검수

1. **작은 기능·장애 검수.** 실제 Windows 앱에서 고정 입력 폴더를 선택한다. 실제 엔진과 저장을 통해 정상·불량·검토 필요·처리 오류가 각각 표시되는지 확인한다. 한글 경로, 같은 파일명, 필터, 중지, DB 단절 후 재시도, 재시작 뒤 조회와 기존 TCP 재전송도 확인한다.
2. **OPENZDM 전체 GUI E2E.** 공식 ZIP을 풀고 앱에서 데이터 루트 폴더를 한 번 선택한다. train 43,169장, validation 9,250장, test 9,251장, **총 61,670장**을 한 실행으로 처리한다. 자동화 도구가 실제 MFC 버튼을 조작하고 MySQL에 저장된 결과를 확인한다. [S02 입력 명세](S02-data-preparation.md)
3. **완료 대조.** 화면의 발견·저장 수와 판정별 건수를 MySQL 집계와 대조한다. S02 manifest와 결과의 상대 경로·파일 해시를 비교해 누락·중복·불일치 0건을 확인한다. 실제 진행·완료 화면과 처리시간을 기록한다. 네 경로의 성능 평가는 [S11 동결 절차](S11-algorithm-integration-evaluation.md)를 따른다.

## 전체 실행 결과

최종 x64 Release 앱에서 OPENZDM 루트 폴더를 한 번 선택했다. Windows 11 ARM VM과 전용 MySQL 8.4.13에서 61,670장 검사·저장까지 **14분 31초**가 걸렸다. 화면의 저장 수와 판정 필터가 MySQL 집계와 일치했다. S02 manifest의 상대 경로와 SHA-256을 모두 대조해 누락·중복·불일치 0건을 확인했다. 앱을 재시작해 같은 검사 이력을 열었고 TCP 재전송 결과와 검사 ID도 확인했다.

| 앱 판정 | 건수 |
|---|---:|
| 정상 | 43,899 |
| 불량 | 11,086 |
| 검토 필요 | 6,685 |
| 처리 오류 | 0 |
| 취소 | 0 |

검토 필요 중 **3,929건**은 시간 예산 초과, **2,756건**은 정밀 점수 불확실이었다. S11의 132µs 설정을 사용했다. 전체 검수용 서버는 디스크 동기화 설정을 `innodb_flush_log_at_trx_commit=2`, `sync_binlog=0`으로 지정했다. 소규모 저장 복구 검수는 MySQL 기본 동기화 설정에서 수행했다. 위 소요 시간은 이 환경의 관측 한 건이다. [실행 중 화면](S13-evidence/full-in-progress.png) · [완료 화면](S13-evidence/full-complete.png) · [전체 검증 기록](S13-evidence/full-verification.json)

## 관리자와 검수자 재현 명령

저장소 루트에서 [S11 x64 빌드 조건](S11-algorithm-integration-evaluation.md#실행과-검수)과 S02 원본 ZIP을 준비한다. 기존 DB를 사용하려면 `db/s05/schema.sql`이 적용된 DB에 관리자가 `db/s13/schema.sql`을 적용한다. Windows VM에서 기존 스테이징 MFC를 사용하는 예는 아래와 같다.

```powershell
$python = (Resolve-Path .tools/python/python.exe).Path
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-s10.ps1 -OutputRoot artifacts/S13 -StagedMfc artifacts/S10/vs-x64 -SelectiveFreeze docs/steps/S11-evidence/freeze.json
& $python -m tools.verify_folder_e2e prepare --output artifacts/S13/OPENZDM
```

검수용 DB는 루프백 MySQL을 사용한다. [접속 변수와 첫 로그인](S12-run-package.md#mysql-연결)을 설정한 셸에서 실행한다. 검증 도구는 전용 DB의 로컬 관리자만 사용해 결과를 조회한다.

```powershell
$app = 'artifacts/S13/x64/Release/WeldScope.App.exe'
& $python -m tools.verify_folder_e2e verify --app $app --fixture-image artifacts/S13/OPENZDM/train/OK/image_10513.png --output artifacts/S13/gui-fixture-new
& $python -m tools.verify_folder_e2e verify --app $app --input artifacts/S13/OPENZDM --output artifacts/S13/gui-full-new
```

소규모 검수는 테스트용 판정 설정으로 실제 PNG 네 장을 처리했다. 정상·불량·검토 필요·처리 오류가 각각 1건이었다. 커밋 응답 유실 뒤 재시도, 검사 중지, 한 장 검사 완료, 앱 재시작 뒤 이력 조회와 같은 TCP 요청의 재전송을 확인했다. [판정 필터와 후보 영상](S13-evidence/04-row-detail.png) · [저장 재시도 화면](S13-evidence/02-storage-retry.png) · [실제 검수 기록](S13-evidence/fixture-verification.json)

S13 스키마가 없는 DB에서는 폴더 검사 준비 상태를 표시하고 단일 파일 검사는 계속 실행된다. 실제 x64 앱으로 기존 스키마에 검사 결과 1건을 저장해 확인했다. 최종 ZIP을 푼 뒤 실행 런처로 전용 DB를 준비하고 동봉된 PNG 3장을 앱에서 폴더 검사했다. 기본 화면과 축소한 창을 96 DPI에서 확인했다.

x64 CTest 8개, Python 일반 검사 65개, 기존 ARM64 단일 파일 실제 창 검사 24개가 통과했다. Python 발견 검사에서 건너뛴 CLI 전용 11개는 CTest에서 실행했다.

패키지는 기존 x64 CLI와 다음 명령으로 만든다. 새 출력 경로를 지정하며, 해제한 폴더 안의 `WeldScope-Start.cmd`로 실행한다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/package-s12.ps1 -FolderInspection -AppDir artifacts/S13/x64/Release -Inspect artifacts/S11/build/Release/weldscope_inspect.exe -TwoStageSample artifacts/S13/OPENZDM/train/OK/image_10513.png -Output artifacts/S13/WeldScope-folder-win-x64-new.zip
```
