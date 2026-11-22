# 버전 2 LightGBM 단일 검사 구현 계획

> **실행 스킬** superpowers:executing-plans 또는 superpowers:subagent-driven-development. 아래 체크박스는 구현 검수용이다.

**목표** 파일, 폴더, CLI, 새 TCP 요청의 검사 경로를 LightGBM 단일 판정으로 통합하고, 이전 알고리즘과 전용 빌드·검수 파일을 제거한다.

**구성** 기존 특징 추출, 220개 결정 나무, 폭별 판정선 보간을 재사용한다. 입력 검증부터 판정까지 공용 C++ 경로 하나를 사용하고, 앱은 검사 제어와 표시, MySQL은 이력, TCP는 요청·응답을 맡는다.

**기술** C++17, OpenCV 4, MFC, MySQL, Winsock, CMake와 MSBuild.

**기준** [버전 2 판정 명세](../../portfolio/README.md#lightgbm-판정-명세), [현행 기본 모델 결정](../../adr/0014-lightgbm-only-inspection.md), [이력과 장비 계약](../../adr/0010-record-device-integration.md), [UI UX 원칙](../../standards/ui-ux.md).

## 변경 계약

| 항목 | 버전 2 |
|---|---|
| 검사 | 기존 모델로 사진 한 장의 점수를 한 번 계산. 정상선 이하 정상, 불량선 이상 불량, 두 기준 사이 검토 필요 |
| 시간 | 분석 소요 시간은 기록값. 132µs 초과가 판정을 바꾸는 분기 제거. 소켓·DB의 통신 대기 제한은 유지 |
| 오류와 취소 | 손상 이미지, 모델 파일 누락·변조, 분석 실패는 오류로 처리. 사용자 중지는 취소로 처리 |
| 새 TCP 요청 | 설정 ID `pooled-smooth-220-v2`만 검사 실행. 이전 등록 ID에 새 요청을 보내면 `CONFIG_UNAVAILABLE`, 미등록 ID는 `UNKNOWN_CONFIG` |
| 과거 요청 | 기존 요청 번호, 입력 ID, 설정 해시가 일치하면 저장된 결과 재전송. 불일치 시 `REQUEST_CONFLICT`. 이전 알고리즘 실행과 모델 파일 로드는 제거 |
| 이력 | 새 판정 버전 `openzdm-pooled-smooth-220-v2`. 기존 MySQL 행과 원본 지문 보존. 과거 상세는 저장된 판정, 점수, 이유와 원본 상태를 조회하며 이전 후보 표시를 다시 계산하지 않음 |

## 작업 1. 변경 계약과 공용 판정 경로

- [x] 현행 ADR 0013의 선택 이유와 이전 요청 재전송 계약을 ADR 0014에 옮긴다. 새 설정 ID, 과거 이력 조회 범위, 신규 응답 계약을 기록하고 로드맵의 이번 작업 참조를 연결한다. 기존 ADR의 삭제와 링크 갱신은 새 계약 적용과 같은 변경에 넣는다.
- [x] `tests/analysis_pipeline_tests.cpp`와 `tests/file_inspection_job_tests.cpp`에 회귀 기준을 먼저 둔다. 두 판정선의 등호, 기준 사이, 한 번의 분석 기록, 132µs 초과에도 같은 점수의 같은 판정, 모델 누락·변조, 손상 PNG와 취소를 검증한다. 기존 코드에서 제거 대상 동작이 드러나는지 확인한다.
- [x] `src/WeldScope.FileInspection/AnalysisPipeline.{h,cpp}`에서 검사 모드 선택, 추가 분석 콜백, 시간 보류 분기를 걷어낸다. `PooledSmoothModel.h`, `CompactBoostedTrees.h`, `InterpolatedScoreBand.h`의 추론과 보간은 재사용한다. 폭별 점수 경계를 표현하는 값 타입과 비교만 남기고, 시간이 판정 함수의 입력이 되는 구조를 제거한다.
- [x] `config/pooled-smooth-220.json`을 버전 2 설정으로 교체한다. 가중치 `config/model-220.txt`와 폭별 경계 수치는 유지하고, 추가 분석·예산 필드와 이전 스키마 전용 파싱을 제거한다. 새 설정과 기존 가중치의 해시 검증을 연결한다.
- [x] `FileInspectionJob.{h,cpp}`와 `src/WeldScope.Cli/main.cpp`를 단일 엔진에 연결한다. CLI의 단일 파일·폴더 평가에 쓰이는 진입점은 유지하고, 이전 모드 선택과 전용 출력 필드를 제거한다. 같은 사진의 파일 검사와 CLI에서 점수·판정·사유가 일치해야 한다.

## 작업 2. 파일·폴더·장비 통합 및 MFC 화면

- [x] `IntegratedInspection.{h,cpp}`의 모드별 등록과 작업 필드를 단일 모델로 바꾼다. 예전 설정 ID와 해시는 과거 재전송 대조에만 사용한다. 조회와 해시 검증이 새 검사 시작보다 먼저 일어나는 기존 순서를 유지한다.
- [x] `InspectionDetails.{h,cpp}`의 예전 후보 마스크 재계산을 제거한다. 기존 행의 시간, 점수, 판정 이유와 이미지의 변경·누락 안내는 유지한다. 새 LightGBM 결과에서 위치별 후보가 제공되는 것처럼 보이는 제어는 정리한다.
- [x] UI 작업에 기존 MFC 검수에서 사용한 Visualize의 직접 라벨 원칙과 [UI UX 원칙](../../standards/ui-ux.md)을 적용한다. `WeldScope.AppDlg.cpp`, 대화상자 리소스와 `resource.h`에서 `정밀 분석 중`, 빠른·정밀 단계 구분, `시간 예산 초과`를 신규 검사 화면에서 제거한다. 오래된 이력의 사유는 이력 상세에서 과거 기록으로 읽을 수 있게 한다.
- [x] 화면에는 `검사 중`, `정상`, `불량`, `검토 필요`를 텍스트로 표시한다. 검토 필요 사유는 `판정 기준 사이`, 독립 필드는 `분석 시간`으로 표시한다. 저장 재시도, 검사 중지, 판정 필터, 원본 조회는 현재 동작을 유지한다. 화면을 다시 배치하기보다 기존 결과·이력 위치를 재사용한다.
- [x] 비통합 MFC 빌드의 `기준 설정 오류` 분기와 샘플 전용 장비 실행기 `src/WeldScope.Device/main.cpp`를 점검한다. 지원하는 빌드 구성에는 같은 LightGBM 기능을 연결하고, 이전 판정만 실행하는 샘플 타깃은 제거한다. TCP 프레임 코덱과 독립 테스트는 공용 기능으로 유지한다.
- [x] 실제 MFC에서 파일과 폴더를 각각 검사하고, 가상 장비에는 새 요청과 과거 재전송을 보낸다. 정상·불량·검토 필요, 오류·취소, DB 저장 실패와 재시작 후 이력 화면을 확인한다. 96 DPI에서 긴 파일명과 축소 창의 키보드 포커스를 확인했고 대표 화면을 남겼다.

- [ ] 125% 이상 확대 화면의 실제 검수. 이번 Windows 세션은 96 DPI였다.

## 작업 3. 이전 구현과 배포 파일 정리

| 삭제 또는 정리 대상 | 삭제 전 확인 |
|---|---|
| `src/WeldScope.Core/BaselineInspection.{h,cpp}`, `LocalContrastInspection.{h,cpp}`, `SelectiveInspectionPolicy.{h,cpp}`, `src/WeldScope.Policy/CMakeLists.txt` | 공용 점수 경계, 결과 표시 등 남은 기능을 새 경로로 연결한 뒤 포함·링크 참조 제거 |
| `src/WeldScope.Cli/SelectiveInspectionDemo.cpp`, `CalibrateS11.cpp`, `config/baseline.json`, `config/local-contrast.json` | 이전 엔진과 선택적 검사 전용 진입점 및 설정. 현재 CLI·앱·패키지에서 참조 제거 |
| `tests/baseline_inspection_tests.cpp`, `local_contrast_inspection_tests.cpp`, `selective_inspection_policy_tests.cpp`, `check_selective_inspection_demo.cmake` | 새 공용 판정과 파일 입력 회귀 검사를 통과한 뒤 이전 모드 전용 테스트 제거 |
| `tools/demo-s04.ps1`, `demo-s07.ps1`, `evaluate_s11.py`, `verify_s11_gui.py` 및 이전 모드 전용 호출자 | 파일별 호출 관계를 확인한다. 현행 도구가 가져다 쓰는 TCP 요청 헬퍼, 데이터 준비 및 빌드 공통 기능은 기존 위치에서 재사용하고 전용 코드만 제거 |
| 예전 설정·동결 파일·실험 실행기 및 중복 증거 | MSBuild 복사, ZIP 입력, 현재 문서 링크와 독립 재현 의존성을 대조한다. 과거 재현은 해당 커밋으로 안내하고, 현재 모델 선택과 실패 사례의 기준 증거는 연구 문서에 남김 |

- [x] `CMakeLists.txt`, `weld-scope.slnx`, `WeldScope.App.vcxproj`, `.vcxproj.filters`, `IntegratedInspection.props`를 새 입력 구성과 일치시킨다. 모든 삭제 대상은 `rg`와 빌드로 남은 참조를 확인한 뒤 제거한다.
- [x] `tools/package-s12.ps1`을 현재 ZIP 패키징에 맞게 단순화한다. 이전 `-Inspect`, `-TwoStageSample`과 전용 설정·동결 파일 복사 의존성을 제거한다. 새 구성에서 `-AppDir`, `-Output`으로 앱, 런처, 현재 설정·가중치, 런타임과 시연 자료를 담는다. 패키지 테스트와 실행 안내도 함께 갱신한다.
- [x] 로드맵과 문서 안내에서 현행과 과거 재현 경로를 정리한다. 삭제한 파일에 대한 링크를 제거한다. 과거 성능의 수치와 실패 사례가 남는 경우 당시 설정의 결과라고 표시한다.

## 작업 4. 최종 검수

- [x] 기존 Visual Studio x64 개발 콘솔에서 공용 C++와 앱을 새 출력 폴더로 빌드한다. 이전 출력 폴더의 DLL이나 JSON을 재사용하면 ZIP 검수가 무효가 될 수 있다.

```powershell
$python = (Resolve-Path .tools/python/python.exe).Path
$opencv = (Resolve-Path .tools/opencv/install-x64/x64/vc18/staticlib).Path
$mysql = (Resolve-Path .tools/mysql-8.4.13-winx64).Path
cmake -S . -B artifacts/lightgbm-v2/build -G 'Visual Studio 18 2026' -A x64 -DWELDSCOPE_S10_TESTS=ON "-DMYSQL_ROOT=$mysql" "-DOpenCV_DIR=$opencv" "-DPython3_EXECUTABLE=$python"
cmake --build artifacts/lightgbm-v2/build --config Release
ctest --test-dir artifacts/lightgbm-v2/build -C Release --output-on-failure
$appOut = Join-Path (Get-Location) 'artifacts/lightgbm-v2/app'
msbuild weld-scope.slnx /p:Configuration=Release /p:Platform=x64 "/p:OutDir=$appOut/"
```

- [x] 갱신한 `tools/verify_portfolio_mfc.py`와 `tools/verify_folder_e2e.py`로 단일 파일, 폴더, 가상 장비, DB 복구 및 재시작을 확인한다. 이전 요청 재전송은 미리 저장한 실제 과거 DB 행의 검사 ID와 판정을 대조한다.
- [x] 같은 동결 시험 사진 8,104장을 버전 2로 검사해 FN, FP, 검토 필요, 처리 오류와 분석 시간을 새로 집계한다. 같은 입력의 반복 판정이 시간 차이로 바뀌지 않는지 확인한다. 이전 수치를 새 성능으로 재사용하지 않는다.
- [x] 최종 ZIP을 새 폴더에 풀고 런처로 실제 앱을 연다. 이전 엔진, 설정, 시연 실행 파일이 ZIP에 없고, 앱 화면과 TCP가 같은 버전으로 판정하는지 확인한다.
- [x] `git diff --check`, 삭제 경로 참조 검색, 문서 링크 검사를 마친다. 빌드·실제 화면·DB·TCP·시험 결과를 해당 기준 문서에서 갱신하고 사용자 검수에 제출한다.

## 실행 결과

[버전 2 검수 기록](../../algorithms/openzdm-false-pass-eda.md#버전-2-검수). x64·ARM64 Release 빌드, CTest 4건, Python 29건, MFC·CLI 8,104장 대조, 파일·폴더·이력·장비 및 ZIP 검수를 실행했다. 이전 경로 전용 파일 70개를 제거했다. 한글 경로에서 실습 MySQL이 시작하지 않아 ZIP 실행 안내는 검수한 영문 경로를 사용한다.
