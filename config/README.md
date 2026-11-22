# 검사 설정

[pooled-smooth-220.json](pooled-smooth-220.json)은 버전 2의 폭별 판정 경계와 모델 지문이다. [model-220.txt](model-220.txt)는 학습된 나무 220개의 가중치다. 파일·폴더·CLI·TCP 검사는 이 두 파일을 사용한다. 로더는 설정과 가중치의 SHA-256을 확인한다. [판정과 호환 계약](../docs/adr/0014-lightgbm-only-inspection.md)
