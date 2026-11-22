# S08 선택적 정밀 분석

> 교체 전 커밋에서 실행한 결과다. 이 문서의 빌드·검수 도구는 [당시 소스](https://github.com/hsjni0110/weld-scope/blob/ce2d6f0/docs/steps/S08-selective-inspection-policy.md)에 있다. 현행 버전 2는 [실행 안내](S13-run-package.md)를 따른다.

**상태**　사용자 결과 검수 대기

**목표**　빠른 점수가 불확실한 경우에만 정밀 분석을 최대 한 번 호출하고, 시간 예산과 결과에 따라 PASS·FAIL·REVIEW·ERROR를 반환한다.

**요구사항**　[로드맵](../ROADMAP.md), [PRD F04·F05](https://chatgpt.com/space/page_b35b9c74caac81918226ccdc9e38769e), [ADR 0008](../adr/0008-selective-inspection-policy.md).

## 방식과 범위

C++17 표준 라이브러리로 구현한 `weldscope_policy`와 고정 관측을 사용하는 `weldscope_policy_demo`를 구성했다. 정책은 점수와 분석 소요 시간, 정밀 분석 콜백을 받는다.

S04는 PASS·FAIL을 계산하고 CLI에서 분석 오류를 기록한다. S07은 세 분석 경로를 각각 실행한다. S08은 정책의 입력으로 점수와 분석 상태를 받아 전이를 검증한다. S03의 `SampleSession`은 화면 시제품의 상태이므로 정책 계약은 공용 Core에 둔다. MFC·실제 영상 연결, 작업 취소, 운영 임계값·추가 분석률 동결과 정확도 평가는 해당 로드맵 단계에서 다룬다.

## 정책 계약

- 높은 점수가 더 강한 이상 후보를 뜻한다. 빠른 단계와 정밀 단계에 각각 `0 ≤ passMax < failMin ≤ 1`인 유한 경계를 둔다. `score ≤ passMax`는 PASS, `score ≥ failMin`은 FAIL이며, 그 사이에서만 추가 분석 또는 REVIEW가 적용된다.
- 시간 예산은 빠른 분석과 정밀 분석의 **합산 분석 시간**이며, 양의 정수 마이크로초로 입력한다. 로드·저장·대기 시간은 별도다. 빠른 분석 시간이 예산을 초과하면 REVIEW다. 빠른 판정이 예산에 정확히 도달했으면 결정된 PASS·FAIL을 반환하고, 불확실하면 정밀 분석을 시작하지 않아 REVIEW다.
- 빠른 점수가 불확실하고 남은 시간이 양수일 때만 동일 입력에 결합된 콜백을 한 번 호출한다. 호출 시 샘플 콘솔이 `REANALYZE`를 표시한다. 정밀 분석이 남은 시간과 같거나 그 전에 끝나면 정밀 점수로 판정하고, 중간 구간이면 REVIEW다. 남은 시간을 초과하면 정밀 점수가 경계 밖이어도 REVIEW다.
- 점수 없음은 분석 실패로 ERROR다. 범위 밖·NaN·무한 점수나 음수 시간도 ERROR다. 분석 실패 또는 잘못된 관측과 예산 초과가 함께 있으면 ERROR를 우선한다. 관측 오류와 예산 보류의 사유를 각각 기록한다.
- 예산 초과 판정은 분석이 반환된 시점의 검사다. 스레드나 강제 중단을 추가하지 않는다. 샘플의 경과 시간은 고정 입력값이며, 실제 연결을 할 때 각 분석 소요 시간은 호출부에서 단조 시계로 측정한다.

### 공용 인터페이스

- `ScoreBand{double passMax, double failMin}`
- `AnalysisMeasurement{std::optional<double> score, std::chrono::microseconds elapsed}`의 두 인자는 필수다. 분석 실패도 점수 없음과 측정한 소요 시간을 함께 전달한다. 측정값이 실제로 0µs라면 0µs를 명시한다.
- `SelectiveInspectionPolicy(ScoreBand fast, ScoreBand precise, std::chrono::microseconds budget)`는 잘못된 경계나 예산에 `std::invalid_argument`를 던진다.
- `SelectiveInspectionPolicy::evaluate(AnalysisMeasurement fast, const std::function<AnalysisMeasurement()>& analyzePrecise) const -> PolicyResult`
- `PolicyResult{InspectionOutcome outcome, PolicyReason reason, std::optional<AnalysisMeasurement> precise}`. `InspectionOutcome`은 `Pass, Fail, Review, Error`다. 사유는 `FastThreshold, PreciseThreshold, PreciseUncertain, BudgetExhausted, FastAnalysisError, PreciseAnalysisError, InvalidFastMeasurement, InvalidPreciseMeasurement`다. `precise`의 유무는 호출 여부를 나타낸다. 콜백은 예상한 분석 오류를 점수 없는 관측으로 변환해 반환한다. 정밀 분석이 필요한데 콜백이 비어 있으면 호출 0회와 `PreciseAnalysisError`를 반환한다. 호출부에서 관측으로 변환하지 않은 예외는 호출부로 전파된다.

## 고정 샘플 실행 결과

빠른 경계는 **0.25 / 0.75**, 정밀 경계는 **0.125 / 0.875**, 합산 예산은 **10,000 µs**로 검수한다. 이 수치의 목적은 경계값을 재현하는 것이며 운영 설정은 S11에서 결정한다.

| 샘플 | 빠른 점수 / 시간(µs) | 정밀 점수 / 시간(µs) | 관찰 판정 / 정밀 호출 |
|---|---|---|---|
| 빠른 정상 경계 | 0.25 / 2,000 | 생략 | PASS / 0 |
| 빠른 불량 경계 | 0.75 / 2,000 | 생략 | FAIL / 0 |
| 정밀 정상 경계·예산 도달 | 0.5 / 2,000 | 0.125 / 8,000 | PASS / 1 |
| 정밀 불량 경계 | 0.5 / 2,000 | 0.875 / 3,000 | FAIL / 1 |
| 정밀 점수 불확실 | 0.5 / 2,000 | 0.5 / 3,000 | REVIEW / 1 |
| 정밀 분석 시작 전 예산 소진 | 0.5 / 10,000 | 생략 | REVIEW / 0 |
| 정밀 분석 1µs 초과 | 0.5 / 2,000 | 0.875 / 8,001 | REVIEW / 1 |
| 정밀 분석 실패 | 0.5 / 2,000 | 점수 없음 / 3,000 | ERROR / 1 |

자동 검사에는 `std::nextafter`로 만든 각 경계 바로 안쪽의 점수를 포함한다. 빠른 단계에서는 정밀 호출 1회, 정밀 단계에서는 REVIEW가 기대된다. 빠른 점수가 확실하면 10,000µs에 PASS·FAIL이며 10,001µs에 REVIEW다. 빠른 실패는 추가 호출 0회, 정밀 실패와 초과가 함께 있으면 ERROR다. 같거나 역전된 경계, 비유한 경계, 0 이하 예산, 범위 밖 점수와 음수 시간도 검사한다.

## 재현과 검수

프로젝트 루트에서 실행한다. S08 전용 CMake 진입점은 S08 정책, 샘플 콘솔과 검사만 구성한다. Windows ARM64용 Visual Studio C++ 도구와 CMake를 사용한다.

```powershell
cmake -S src/WeldScope.Policy -B artifacts/S08/build -G 'Visual Studio 18 2026' -A ARM64
cmake --build artifacts/S08/build --config Release
ctest --test-dir artifacts/S08/build -C Release --output-on-failure
.\artifacts\S08\build\Release\weldscope_policy_demo.exe
```

[실제 콘솔 출력](S08-evidence/sample-console.txt)에 빠른 점수·시간, 정밀 호출 횟수와 소요 시간, 최종 판정과 사유가 있다. 콘솔 CTest는 실행 파일을 두 번 호출해 UTF-8 출력과 종료 코드를 검증한다. 8개 중 5개에서 `REANALYZE`가 콜백 시작 시 1회 출력됐다. 콘솔에는 `[샘플]`과 판정·사유를 텍스트로 표시했다. UI UX 원칙과 Visualize의 직접 라벨 지침을 적용해 색상 없이 상태를 구별할 수 있다.

- Windows ARM64 Release에서 S08 독립 CTest 2개와 기존 통합 빌드의 CTest 5개가 통과했다. S08 파일만 있는 복사본에서도 독립 CTest가 통과했다. 정책 검사는 소요 시간 생략의 컴파일 차단, 0·1 점수, 판정 경계, 잘못된 관측, 최대 마이크로초와 빈 콜백을 포함한다.
- 별도 Python 테스트는 44개 통과했다. 실제 CLI 검사 7개는 CTest에서 실행했다. S07 고정 16장도 세 경로로 다시 실행해 점수·판정과 PNG 192개 일치를 확인했다. 국소 대비 설정의 원시 해시는 Git의 줄 끝 변환으로 달라졌으며, 설정 내용과 각 결과의 실제 파일 해시 기록을 각각 확인했다.
- 콘솔과 정책 검사에 중복된 대표 사례는 콘솔 CTest가 검증한다. 복사본의 `REANALYZE` 출력문을 제거하면 시연은 코드 0으로 종료하지만 CTest는 출력 차이를 감지해 실패한다.

사용자 검수 항목은 위 표의 판정과 정밀 호출 0·1회, 시간 10,000µs와 10,001µs의 차이, 오류 사유다. 샘플의 시간은 고정 관측값이며 실제 영상 연결과 운영 성능 평가는 S11 범위다.
