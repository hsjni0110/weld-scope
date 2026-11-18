# S02 데이터 준비

상태: 사용자 검수 대기. 2026-11-17 승인된 설계를 구현하고 공식 원본 전체 점검과 재실행 비교를 수행했다. 검사 성능 평가용 사용은 아래 제한 사항 검수 전까지 보류한다.

## 목표와 범위

OPENZDM 공개 용접 외관 데이터를 점검하고, 같은 입력으로 재생성 가능한 파일 목록과 정상/불량 샘플 접촉표를 제공한다. S02만 수행하며 MFC 화면, 검사 알고리즘, 학습, DB, TCP는 변경하지 않는다. 데이터 채택과 분할 결정은 [ADR 0003](../adr/0003-dataset-preparation.md)에서 관리한다.

## 접근 방식과 선택

| 방식 | 장점 | 비용과 선택 |
|---|---|---|
| PowerShell과 .NET | Windows 기본 기능 활용 | Mac에서 동일 실행이 어려움. 미선택 |
| Python과 Pillow | Windows와 Mac에서 같은 준비 절차 사용 | Python 환경과 이미지 디코더 설치 필요. 설계와 함께 사용자 승인 |

Python 표준 라이브러리로 다운로드, ZIP, 해시, CSV, JSON을 처리하고 Pillow로 이미지 전체 디코딩, 픽셀 검사, 축소 및 접촉표를 처리한다. 구현은 [준비 스크립트](../../tools/prepare_openzdm.py), [의존성](../../tools/requirements-s02.txt), [자동 검사](../../tests/test_prepare_openzdm.py)이다. 실제 실행 환경은 Windows 11 ARM64 build 26300, CPython 3.13.9 ARM64, Pillow 13.0.0이다. Python이 설치되어 있지 않아 프로젝트 내부 `.tools/python/`에 공식 embeddable 배포판과 해시 검증한 Pillow wheel을 격리했다. 시스템 Python·PATH는 변경하지 않았다. Mac 실행은 미검증이다.

## 데이터와 사용 조건

2026-11-17에 [Zenodo 공식 레코드](https://zenodo.org/records/14025630)와 [공식 API](https://zenodo.org/api/records/14025630)를 조회했다.

| 항목 | 공식 메타데이터 |
|---|---|
| 제목 | 2D image dataset for the presence or absence of aesthetical defects on EV battery welds. |
| 작성자 | OPENZDM |
| DOI | 10.5281/zenodo.14025630 |
| 접근과 라이선스 | open, cc-by-4.0 |
| 파일 | Weld Aesthetical Defects.zip |
| 크기 | 1,153,183,844 bytes |
| MD5 | f187dac063cf656bb627327fa9771d16 |

공식 설명은 흑백 409×40 이미지 61,670장, train 43,169장, validation 9,250장, test 9,251장과 OK/KO 이미지 단위 라벨이다. 이는 설명의 수치이며 로컬 파일 검산 결과가 아니다. 픽셀 마스크나 부품 그룹 ID의 존재는 확인되지 않았다.

[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)에 따라 공개 산출물에 작성자, 제목, 원본 링크, 라이선스 링크와 가공 여부를 표시한다. ZIP과 전체 원본은 Git에서 제외한다. 접촉표는 축소·배치한 가공물로 출처를 함께 표시한다. API 메타데이터와 로컬 ZIP의 SHA-256도 재현 근거로 보관한다.

## 실행 흐름과 산출물

1. 전용 Python 가상환경을 준비한다. 기존 ZIP을 지정하거나 공식 URL에서 내려받는다. 기존 파일을 조용히 덮어쓰지 않고 다운로드 임시 파일의 크기와 MD5를 확인한 뒤 확정한다.
2. ZIP 항목의 경로 이탈·중복 경로·링크·비정상 크기를 검사한다. 실제 항목 구성을 확인한 뒤 split과 label 매핑을 명시한다. 알 수 없는 구조나 라벨을 정상으로 추정하지 않는다.
3. 모든 이미지의 디코딩, 크기, 모드, 파일 SHA-256과 디코딩 픽셀 해시를 검사한다. 픽셀 해시는 크기와 RGBA로 통일한 픽셀 바이트를 포함해 저장 형식·색상 모드 차이와 실제 픽셀 차이를 구분한다. 원본은 변환하지 않는다. 공식 설명과 다른 크기·모드는 손상이 아니라 설명 불일치로 집계한다. 파일 수와 split/label별 수량을 공식 수치와 대조한다. 손상·비이미지·예상 밖 항목도 목록과 오류에 남긴다.
4. 동일 파일과 동일 픽셀 그룹, 분할 간 중복, 서로 다른 라벨의 충돌을 보고한다. 축소 밝기 변화 해시(dHash, 64비트)의 해밍 거리 5 이하를 유사 이미지 후보로 보고한다. 유사 후보는 중복 확정이나 자동 삭제 근거로 사용하지 않는다. 판정 기준과 검색 범위·후보 수를 보고서에 기록한다.
5. 원본 분할을 유지한 정렬된 manifest와 점검 요약을 만든다. 이미지 상대 경로, 공식 split, label, 크기, 모드, 파일 크기, 파일/픽셀 해시와 점검 상태를 기록한다. 실행 시각은 요약에만 기록해 manifest 재실행 결과를 비교할 수 있게 한다.
6. 정상/불량을 각각 고정 경로 순서로 선택한 train 샘플 접촉표를 만든다. 원본 비율을 유지하고 파일 식별자와 라벨, “샘플”, 출처를 표시한다. test는 접촉표와 특징 탐색에 사용하지 않는다.

로컬 대용량 파일은 `data/openzdm/`, 재생성 결과는 `artifacts/S02/`에 둔다. 아래에 연결한 공유 증거는 `docs/steps/S02-evidence/`에 보관했다. 원본 ZIP은 추출하거나 수정하지 않았다. manifest의 `path`는 ZIP 내부의 원본 상대 경로이며 `val` 경로를 보존하고 `split`만 `validation`으로 표시한다. 후속 파일 기반 단계에서는 검증된 ZIP을 로컬에 추출해 사용해야 한다.

접촉표는 visualize 스킬의 정적 그림 제작 지침과 프로젝트 UI UX 원칙을 적용한다. 정상/불량을 색상 대신 한국어와 원본 라벨로 구분하고, 두 열의 행 높이를 맞추며, 원본 종횡비와 읽을 수 있는 파일명을 유지한다. 실데이터 접촉표를 이미지 뷰어로 열어 글자 잘림과 라벨·영상 대응을 검수한다. 별도 웹 앱은 만들지 않는다.

## 분할과 평가 제약

공식 분할을 보존하며 임의의 재분할, 원본 삭제, 자동 중복 제거는 하지 않는다. train은 개발, validation은 정책 조정, test는 설정 동결 후 최종 평가에 사용한다. S02의 test 접근은 파일 무결성과 누수 점검으로 한정한다.

중복이나 라벨 충돌이 발견되면 원본 manifest와 문제 목록을 남기고 평가 사용을 보류한다. 실제 문제를 확인한 뒤 제외·그룹화 정책과 별도 manifest를 사용자에게 검수받는다. 부품 그룹 ID가 없으면 제품 단위 독립성을 검증했다고 보고하지 않는다. 유사도 기준을 통과해도 제품 단위 누수가 없다는 증거로 해석하지 않는다.

## 수락 기준과 검증

- 출처·이용 조건·공개 가능 범위와 ZIP 체크섬의 근거가 존재한다.
- 모든 입력 항목을 집계하고 split/label 수량, 손상, 동일 파일, 동일 픽셀, 유사 후보와 라벨 충돌을 보고한다.
- 작은 합성 입력으로 손상 이미지, 중복, 라벨 충돌, ZIP 경로 이탈을 검증한다. 합성 입력의 통과를 실제 데이터 점검으로 보고하지 않는다.
- 같은 ZIP과 설정의 재실행에서 manifest와 샘플 선택이 동일하다.
- 사용자가 정상/불량 train 샘플을 나란히 확인하고 로컬 실행을 재현할 수 있다.
- 오류·누수·미확인 그룹 정보는 별도 제한으로 남고 다음 단계 진행 여부는 사용자 검수로 결정한다.

## 실제 결과와 사용자 검수

### 2026-11-17 전체 파일 검산

| 분할 | OK | KO | 합계 |
|---|---:|---:|---:|
| train | 34,916 | 8,253 | 43,169 |
| validation (`val`) | 7,482 | 1,768 | 9,250 |
| test | 7,482 | 1,769 | 9,251 |
| 전체 | 49,880 | 11,790 | 61,670 |

ZIP에는 이미지 61,670개와 디렉터리 9개가 있다. 전 이미지 디코딩 성공, 손상·미지원 항목 0개, 파일 및 동일 픽셀 중복 그룹 0개, 분할 간 완전 중복 0개, 완전 중복의 라벨 충돌 0개다. 라벨의 실제 의미가 모두 올바르다고 검증한 것은 아니다.

중요한 공식 설명 차이: 실제 61,670장은 모두 RGB 저장이며 가로 48–651픽셀, 세로 22–70픽셀이다. 크기/모드 조합은 3,355종이다. RGB 저장은 영상 내용이 유채색이라는 뜻이 아니다. 고정 409×40 입력으로 가정하거나 원본을 임의 리사이즈하지 않았다. 상세 분포는 [점검 요약](S02-evidence/summary.json)의 `image_properties`에서 확인한다.

dHash 유사 후보는 전체 33,253쌍, 분할 간 15,032쌍이다. 61,652개 고유 dHash 사이의 31,682개 관계와 같은 dHash 내부 19쌍을 그룹 단위로 저장했다. 서로 다른 그룹의 관계는 그룹 크기의 곱만큼 이미지 쌍을 뜻하므로 단순 행 수와 전체 후보 수는 다르다. 모든 후보를 저장했고 결과를 잘라내지 않았다. 유사 후보는 같은 부품·같은 촬영의 확정 증거가 아니며 저해상도 밝기 패턴에 의한 오탐을 포함할 수 있다. 원본을 삭제하거나 재분할하지 않았다.

최초 일괄 감사는 약 62.91초에 완료됐다. 유지보수 리뷰 반영 후 전체 감사는 약 47.86초에 완료됐으며 현재 요약은 이 실행 결과다. 현재 `duration_seconds`는 접촉표 생성과 ZIP 체크섬 시간을 제외한 데이터 감사 시간이다. VM에서 수행한 준비 시간이며 검사 엔진의 성능이나 이번 수정의 속도 개선 근거로 사용하지 않는다. 최초 2회 실행과 리뷰 반영 후 실행에서 시각·소요시간 외 요약, manifest와 모든 고정 산출물이 동일했다.

### 공유 증거

- [정상/불량 접촉표](S02-evidence/contact_sheet.png): train OK/KO 각 8장, 총 16장. 실제 이미지 뷰어에서 라벨, 파일명, 종횡비, 출처와 글자 잘림을 확인했다. test 샘플은 포함하지 않는다.
- [manifest](S02-evidence/manifest.csv): 61,670행. 원본 상대 경로, 분할, 라벨, 크기·모드, 파일·픽셀 해시, dHash, 점검 상태.
- [점검 요약](S02-evidence/summary.json), [완전 중복 목록](S02-evidence/exact_duplicates.json).
- [dHash 그룹](S02-evidence/dhash_groups.json), [그룹 간 유사 후보](S02-evidence/similar_hash_pairs.csv). 같은 그룹의 경로 조합은 내부 후보, CSV에 연결된 두 그룹 경로의 곱집합은 그룹 간 후보다.
- [샘플 선택 목록](S02-evidence/contact_selection.json), [ZIP 체크섬](S02-evidence/archive-checksum.json), [공식 API 응답](S02-evidence/source-metadata.json), [재실행 비교](S02-evidence/reproducibility.json).
- [유지보수 개선 검증](S02-evidence/maintenance-verification.json): 기존 산출물과 비교 및 접촉표 단독 재시도 결과.

### 실행 재현

현재 작업 폴더에서 실제 실행한 PowerShell 명령이다. 원본 ZIP 약 1.15GB는 Git에 포함하지 않는다. 전체 감사의 출력 디렉터리는 비어 있어야 한다.

```powershell
.tools/python/python.exe -W error -m unittest discover -s tests -v
.tools/python/python.exe -W error tools/prepare_openzdm.py --archive 'data/openzdm/Weld Aesthetical Defects.zip' --output artifacts/S02/run-01
.tools/python/python.exe -W error tools/prepare_openzdm.py --archive 'data/openzdm/Weld Aesthetical Defects.zip' --output artifacts/S02/run-02
Get-FileHash artifacts/S02/run-01/manifest.csv, artifacts/S02/run-02/manifest.csv -Algorithm SHA256
.tools/python/python.exe -W error tools/prepare_openzdm.py --output artifacts/S02/run-review
.tools/python/python.exe -W error tools/prepare_openzdm.py --contact-sheet-only --output artifacts/S02/retry-review
```

위 경로는 이미 결과가 있으므로 전체 감사를 다시 실행할 때는 새 출력 경로를 사용한다. `retry-review`는 `run-review`의 manifest·요약·체크섬만 복사한 폴더에 접촉표를 단독 생성한 실제 검증이다. 이 경로 역시 이미 접촉표가 있으므로 다시 실행하면 덮어쓰기 방지 오류가 난다. `.tools`가 없는 새 환경의 설치 예제는 다음과 같다. 이 가상환경 생성·설치 조합은 이번 세션에서 실행하지 않았으며 위의 embeddable 환경만 실제 검증했다.

```powershell
python -m venv .venv
.venv/Scripts/python.exe -m pip install -r tools/requirements-s02.txt
.venv/Scripts/python.exe tools/prepare_openzdm.py --output artifacts/S02/new-run
```

`--archive` 생략 시 공식 API와 라이선스·파일 메타데이터를 확인해 다운로드한다. 부분 파일은 HTTP Range로 이어받고, 크기·MD5가 일치해야 원본 ZIP으로 확정한다. 이번 최초 전송 정체는 받은 부분을 보존한 구간 다운로드로 복구했고, 완료된 ZIP을 실제 CLI의 체크섬 검증에 통과시켰다. 일반 다운로드 경로의 빈 부분 파일 재개와 서버의 Range 무시는 로컬 HTTP 서버 자동 검사로 검증했다.

다운로드 경로는 검증된 SHA-256을 반환해 CLI가 재사용하며, 명시적 ZIP 입력도 검증은 한 번만 수행한다. 데이터 감사는 글꼴에 의존하지 않고 요약·manifest를 저장한다. 이후 접촉표에 실패하면 감사와 체크섬을 보존한 채 오류를 표시한다. 원인을 해결한 뒤 같은 `--output`에 `--contact-sheet-only`를 추가하면 전체 디코딩 없이 접촉표만 재시도한다. 완료된 접촉표는 덮어쓰지 않는다.

종료 코드 0은 요청 작업 완료, 1은 다운로드·체크섬·출력·접촉표 등 실행 오류, 2는 전체 감사와 접촉표 생성 완료 후 잘못된 입력 발견이다. 접촉표 실패 시에는 코드 1이어도 완료된 데이터 감사가 남을 수 있다. 종료 코드 0이어도 `evaluation_ready=false`일 수 있으며 이번 결과가 해당한다. 평가 보류 사유는 `summary.json`에 남는다.

### 검증과 남은 제한

- [자동 검사 19개 통과](S02-evidence/test-results.txt), 경고를 오류로 처리한 실행에서도 통과. 기존 14개에 글꼴 실패 후 감사 보존·접촉표 재시도, 그룹별 실제 후보 수, CLI 체크섬 재사용, 기존 ZIP 단일 검증, 접촉표 단독 CLI 검사를 추가했다. 후보 집계를 고의로 -1로 반환했을 때 새 집계 테스트가 실패하는 것도 확인했다.
- 독립 코드 검토에서 지적된 빈 부분 파일 재개와 거대 PNG 감사 중단을 실패 재현 후 수정했다. 전체 검토 기록과 실행 중 판단은 [구현 기록](S02-implementation-plan.md)을 따른다.
- 유지보수 리뷰의 네 항목은 작성자 검토와 회귀·실데이터 비교로 확인했다. 수정 후 별도 검토자의 소스 읽기가 완료되지 않아 독립 재검토 완료로 보고하지 않는다.
- 완전 중복은 없지만 유사 후보와 제품 그룹 미확인으로 부품 단위 독립성을 보장하지 않는다. 원본 속성 차이의 수용, 유사 후보의 해석과 정제·평가 정책을 사용자 검수에서 결정해야 한다.
- 설계와 ADR은 승인됐으며 S02 결과의 최종 검수는 대기다. S03 이후 기능은 구현하지 않았다. Mac 실행, 검사 정확도, 픽셀 정답과 실제 장비 성능은 검증하지 않았다.
