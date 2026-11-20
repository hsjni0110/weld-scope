# 아키텍처 결정 기록

## ADR 대상

여러 모듈, 장기 유지보수, 호환성 또는 외부 계약에 영향을 주는 선택을 ADR로 관리한다. 예: 실행·빌드 환경, 엔진과 화면의 의존성, 데이터와 평가 분할, 판정 상태, 자원 소유권·동시성, TCP 계약, DB 멱등성, 핵심 의존성 도입.

단순 변수명 변경, 국소 버그 수정, 현재 규칙을 따르는 구현마다 ADR을 만들지 않는다.

## 절차

1. 관련 현행 ADR과 실제 코드를 읽는다.
2. 중요한 결정은 구현 전에 [템플릿](TEMPLATE.md)으로 문제, 대안, 선택 이유, 영향과 검증 방법을 작성한다.
3. 이미 사용자가 확정한 선택은 승인 사실과 근거를 기록한다. 새 중요 결정은 제안 상태로 사용자 검수에 포함한다.
4. 검수된 결정을 적용하고 시연·검증 결과를 기록한다. 결정 승인과 구현 성공을 구분한다.
5. 결정이 바뀌면 새 번호를 사용한다. 필요한 이유를 새 ADR에 통합하고 낡은 ADR을 삭제한다. 참조와 아래 목록을 갱신한다.

상태는 제안 또는 승인이다. 승인 전 구현 의존성을 확정하지 않는다. 기각된 안은 채택 ADR의 대안 항목에 이유를 남기고 별도 낡은 문서는 제거한다.

## 현행 ADR

| 번호 | 결정 | 상태 | 구현 검증 |
|---|---|---|---|
| [0002](0002-development-environment.md) | UTM의 Windows ARM 개발 환경 | 승인 | 미검증 |
| [0003](0003-dataset-preparation.md) | OPENZDM 데이터 준비와 공식 분할 보존 | 승인 | 19개 자동 검사, 61,670장 전체 점검 및 재현 확인. 실제 결과의 사용자 검수 대기 |
| [0004](0004-baseline-inspection.md) | C++ 기준 검사와 train 개발 실행 | 승인 | ARM64 빌드, 실제 CLI·오류 검사, 전체 train 2회와 재현 확인. 사용자 검수 후 main 병합 |
| [0005](0005-inspection-history.md) | MySQL 검사 이력과 재전송 규칙 | 제안 | x64 독립 콘솔과 MySQL 8.4.13에서 검증 후 main 병합 |
| [0006](0006-virtual-device-protocol.md) | 가상 장비 TCP 요청과 재전송 | 승인 | ARM64 코덱 검사 및 루프백 시연 통과. 사용자 결과 검수 대기 |
| [0007](0007-local-contrast-inspection.md) | 국소 대비와 다중 스케일 검사 | 승인 | [S07 단계 기록](../steps/S07-improved-inspection.md) |
| [0008](0008-selective-inspection-policy.md) | 선택적 정밀 분석 정책과 시간 예산 | 승인 | [S08 단계 기록](../steps/S08-selective-inspection-policy.md) |
| [0009](0009-mfc-file-inspection.md) | MFC 파일 검사와 작업 수명 | 승인 | [S09 단계 기록](../steps/S09-file-inspection-integration.md) |
| [0010](0010-record-device-integration.md) | 기록과 장비 통합 계약 | 승인 | [S10 단계 기록](../steps/S10-record-device-integration.md) |
| [0011](0011-algorithm-integration-evaluation.md) | 공용 검사 호출과 S11 평가 기준 | 승인 | [S11 실행 기록](../steps/S11-algorithm-integration-evaluation.md) |
| [0012](0012-folder-inspection-monitoring.md) | 폴더 검사와 묶음별 결과 저장 | 승인 | [S13 실행과 검수](../steps/S13-folder-inspection-monitoring.md) |

엔진 경계, 상태 계약, TCP·DB 계약은 해당 단계에서 ADR로 구체화한다. 표에 없는 제안을 승인된 결정으로 간주하지 않는다.
