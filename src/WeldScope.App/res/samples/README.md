# S03 샘플 출처

OPENZDM, *2D image dataset for the presence or absence of aesthetical defects on EV battery welds.*

- 원본: https://zenodo.org/records/14025630 (DOI: 10.5281/zenodo.14025630)
- 라이선스: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)
- `normal.png`: `train/OK/image_1.png`
- `defect.png`: `train/KO/image_1.png`
- 원본 PNG 바이트는 변경하지 않았다. 앱에서 원본 비율로 확대·축소한다.
- 후보 사각형과 정상·불량·검토 필요 판정 전환은 사전 지정한 화면 시연이다. 실제 검출·평가 결과가 아니다.
- `corrupt.png`는 오류 복구 확인용 합성 비이미지이며 원본 데이터가 아니다.
- 파일별 SHA-256과 크기는 `provenance.json`에서 확인한다.

이 폴더는 S03의 출처 기록과 S04·S09 회귀 검사에 쓰이는 파일을 보관한다. S09 앱은 선택한 파일을 읽는다. 이 파일을 만든 [샘플 준비 스크립트](https://github.com/hsjni0110/weld-scope/blob/94be8e2c663941eeb427ade6eaa219d38a03d049/tools/prepare_s03_assets.py)는 당시 커밋에서 확인할 수 있다. 재생성에는 로컬 S02 ZIP이 필요하다.
