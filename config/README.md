# 검사 설정

[baseline.json](baseline.json)은 S04 실행 설정의 기준이다. 키의 의미, 판정 수식과 선택 근거는 [전역 어두운 픽셀 비율](../docs/algorithms/global-dark-ratio.md)에 있다.

[local-contrast.json](local-contrast.json)은 S07 빠른·정밀 분석의 공통 개발 설정이다. [S07 검사 방식과 비교 결과](../docs/steps/S07-improved-inspection.md)를 참조한다.

설정 변경 후 실행할 때는 새 출력 디렉터리를 사용한다. 결과에 기록된 설정 파일의 SHA-256으로 실행을 구분한다.
