# S12 전달과 재현

**상태**　구현 및 로컬 실행 검증 완료 · 사용자 결과 검수 대기

**구현 기준**　`307112c` · S11 병합

[PRD F09와 최종 산출물](https://chatgpt.com/space/page_b35b9c74caac81918226ccdc9e38769e) · [로드맵](../ROADMAP.md) · [S11 검증 기록](S11-algorithm-integration-evaluation.md)

S11의 x64 Release 빌드와 동결 설정을 재사용했다. 실행 ZIP과 평가 재현 ZIP은 로컬의 `artifacts/S12`에 있다. 외부 공개 범위는 프로젝트 및 연결 라이브러리의 이용 조건을 확인한 뒤 결정한다. [의존성과 영상 출처](S12-third-party-notices.md)

## 실행 패키지 생성

[Visual Studio x64 C++ 도구, OpenCV와 MySQL](S11-algorithm-integration-evaluation.md#실행과-검수), [S02 원본 ZIP](S02-data-preparation.md)을 준비하고 저장소 루트에서 실행한다. Visual Studio에 x64 MFC가 설치돼 있으면 앱 빌드 명령의 `-StagedMfc artifacts/S10/vs-x64`를 생략한다. 출력 경로는 새 파일을 지정한다.

```powershell
& {
    $ErrorActionPreference = 'Stop'
    $python = (Resolve-Path .tools/python/python.exe).Path
    $opencv = (Resolve-Path .tools/opencv/install-x64/x64/vc18/staticlib).Path
    $sample = 'artifacts/S12/image_10513.png'
    New-Item -ItemType Directory -Path 'artifacts/S12' -Force | Out-Null
    & $python -c 'from pathlib import Path; from zipfile import ZipFile; import sys; Path(sys.argv[2]).write_bytes(ZipFile(sys.argv[1]).read(sys.argv[3]))' 'data/openzdm/Weld Aesthetical Defects.zip' $sample 'train/OK/image_10513.png'
    if ($LASTEXITCODE -ne 0) { throw 'Sample extraction failed.' }
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-s10.ps1 -StagedMfc artifacts/S10/vs-x64 -SelectiveFreeze docs/steps/S11-evidence/freeze.json
    if ($LASTEXITCODE -ne 0) { throw 'MFC build failed.' }
    cmake -S . -B artifacts/S12/cli-build -G 'Visual Studio 18 2026' -A x64 "-DOpenCV_DIR=$opencv" "-DPython3_EXECUTABLE=$python"
    if ($LASTEXITCODE -ne 0) { throw 'CLI configuration failed.' }
    cmake --build artifacts/S12/cli-build --config Release --target weldscope_inspect
    if ($LASTEXITCODE -ne 0) { throw 'CLI build failed.' }
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/package-s12.ps1 -AppDir artifacts/S11/x64/Release -Inspect artifacts/S12/cli-build/Release/weldscope_inspect.exe -TwoStageSample $sample -Output artifacts/S12/WeldScope-win-x64-new.zip
    if ($LASTEXITCODE -ne 0) { throw 'Package creation failed.' }
}
```

`WeldScope-win-x64.zip`에는 MFC 앱과 검사 CLI, 동결 설정, 시연용 PNG, DB 스키마, 실행 안내, 라이선스 원문과 파일별 해시가 있다. 빌드가 실패하면 패키지 생성을 중단한다. 패키저는 두 실행 파일의 x64 PE 헤더와 한 장만 추출한 시연용 PNG의 해시를 검사한다. 목록의 파일 15개를 해시로 대조했고 한글과 대괄호가 포함된 경로에서도 패키지를 만들었다.

## 패키지 실행 검증

[패키지 실행 안내](S12-run-package.md)에 따라 저장소 밖에서 공백과 한글이 있는 경로에 압축을 풀었다. 별도 현재 작업 디렉터리와 Windows 시스템 PATH를 사용했고, Windows에 설치된 VC++ 런타임과 원본 MySQL DLL 세 개를 사용했다. 빈 전용 루프백 MySQL 스키마에 연결했다.

| 시연 | 실제 관찰 |
|---|---|
| 정상 라벨 이미지 | `samples/normal.png`가 이 실행에서 `검토 필요`로 저장됐다. 정상 라벨과 검사 판정은 별도로 유지된다. |
| 정밀 단계 | `image_10513.png`가 FAST와 PRECISE를 실행했고 두 단계가 MySQL에 저장됐다. |
| TCP와 이력 | 실행 안내의 PowerShell 요청으로 `RESULT`를 받았다. 같은 ID의 재전송과 앱 재시작 뒤 같은 결과를 받았고 DB에 한 행만 남았다. 기존 GUI 검사에서 저장 이력도 열었다. |
| 오류와 재연결 | DB가 내려간 상태에서 시작이 제한됐다. DB 재시작 후 앱 계정으로 첫 접속을 확인하고 `재연결`했다. 손상 입력을 `처리 오류`로 저장하고 정상 입력 검사로 복귀했다. |

환경은 Windows 11 ARM QEMU VM 빌드 26300, 4 vCPU, 8GiB 게스트 메모리, MSVC 14.52.37029.0 x64 Release, OpenCV 4.13.0, MySQL 8.4.13이다. CLI는 OpenCV 1스레드로 실행했다. 검증은 기존 VM에서 경로와 DB를 격리해 수행했다. 별도 Windows 머신의 설치 검증은 남아 있다. 개발 VM 결정은 UTM을 기록한 ADR 0002를 따른다. PRD의 VMware 서술은 기존 결정과 대조할 항목으로 남긴다. [DB 연결 끊김 화면](S12-evidence/05-db-disconnected.png) · [패키지 실행 관측](S12-evidence/package-recovery.json) · [MFC·TCP 및 MySQL 검증](S12-evidence/verification.json) · [VM 결정](../adr/0002-development-environment.md)

## 평가 재현

`WeldScope-S11-evaluation.zip`에는 `evaluation/` 아래 원래 설정·manifest·검증 선택·동결 기록과 원시 시험 CSV 및 요약 8쌍, `original/` 아래 당시 x64 CLI와 보정기, `replay/` 아래 S12에서 별도로 실행한 결과가 있다. 고지와 라이선스 원문을 포함해 파일 51개를 SHA-256으로 확인했다. 기존 CSV 8개를 다시 집계한 값은 저장된 공식·주 시험 보고서와 일치했다.

ZIP을 다시 만들려면 당시 바이너리와 원시 평가 자료를 보존한 폴더를 지정한다. 이 자료는 Git에서 제외되므로 로컬 보관본도 함께 유지한다. 내보내기는 동결 해시와 CSV 집계를 검증하며 다른 실행 환경에서도 가능하다. `--replay`는 S12 재실행 자료를 추가할 때 사용한다. ZIP과 원본 증거를 모두 잃은 경우에는 새 검증과 동결로 별도 평가 기록을 만든다.

```powershell
$python = (Resolve-Path .tools/python/python.exe).Path
& $python -m tools.export_s12_evaluation --evaluation artifacts/S11/evaluation --inspect artifacts/S11/original-ea015e2/weldscope_inspect.exe --calibrator artifacts/S11/original-ea015e2/weldscope_calibrate_s11.exe --replay artifacts/S12/replay --output artifacts/S12/WeldScope-S11-evaluation-new.zip
```

기존 시험을 다시 실행할 때는 저장소 루트에서 ZIP을 새 폴더에 풀고 [S02 원본 ZIP](S02-data-preparation.md)을 지정한다. 기존 실행 파일 해시와 동결 기록의 OS 문자열, CPU 식별자, 게스트 아키텍처 및 논리 CPU 수가 모두 일치해야 한다. 이 기록은 Windows 11 ARM QEMU VM 빌드 26300과 4 vCPU를 사용했다. 다른 조건에서는 [새 검증과 동결](S11-algorithm-integration-evaluation.md#실행과-검수)을 거친다.

```powershell
$python = (Resolve-Path .tools/python/python.exe).Path
$bundle = 'artifacts/S12/evaluation-unpacked'
Expand-Archive -LiteralPath artifacts/S12/WeldScope-S11-evaluation.zip -DestinationPath $bundle
& $python -m tools.evaluate_s11 test --output "$bundle/evaluation" --inspect "$bundle/original/weldscope_inspect.exe" --archive 'data/openzdm/Weld Aesthetical Defects.zip' --test-output "$bundle/replay-new"
```

S12 재실행에서 공식 시험 9,251장과 주 시험 8,104장에 네 검사 경로를 두 차례 적용했다. 선택적 경로의 주 시험 KO 자동 통과는 753건과 748건이었다. 최초 동결 실행의 754건과 752건은 S11 기록에 보존했다. 평가 ZIP을 풀고 원본 ZIP에서 시험 영상을 다시 추출하는 위 절차도 완료했다. 시간에 영향을 받는 선택적 판정은 같은 설정에서도 달라질 수 있다. 수정 실행 파일의 평가에는 [새 검증과 동결 절차](S11-algorithm-integration-evaluation.md#실행과-검수)를 적용한다.

현재 패키지 CLI로 같은 train 영상에 네 경로를 실행했다. 기준은 FAIL, 빠른 분석은 FAIL, 전수 정밀은 PASS, 선택적 분석은 REVIEW였으며 각 경로에서 CSV와 후보 오버레이 PNG를 생성했다. 이 시연은 내보내기 검증에 사용했다.

리뷰 반영 후 실행 ZIP을 새 x64 CLI 빌드로 생성했고, 압축을 푼 CLI에서 CSV를 생성했다. 빌드 실패 시 패키징에 진입하지 않는 것을 확인했다. 평가 ZIP은 파일 51개가 보관본과 바이트 단위로 일치했고, 압축을 푼 자료에서 동결 해시와 환경 검사가 통과했다.

CTest 7개와 Python 일반 검사 65개가 통과했다. Python에서 건너뛴 CLI 전용 검사 11개는 CTest에서 실행했다. [프로젝트 소개](../../README.md)
