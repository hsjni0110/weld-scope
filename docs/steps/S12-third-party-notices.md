# S12 의존성 및 출처

| 구성 | 확인한 버전과 고지 |
|---|---|
| Microsoft Visual C++ 및 MFC | MSVC v145로 빌드했고 실행 검수에서는 v14.52.37029.0 런타임을 사용했다. Windows용 [공식 x64 설치본](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)을 설치한다. ZIP의 앱은 `mfc140u.dll`, `MSVCP140.dll`, `CONCRT140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`을 동적으로 사용한다. [Visual Studio 2026 배포 목록](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution) |
| Oracle MySQL Community 8.4.13 | [공식 MySQL 설치본](https://dev.mysql.com/downloads/mysql/8.4.html)의 `lib/libmysql.dll`, `bin/libssl-3-x64.dll`, `bin/libcrypto-3-x64.dll`을 실행 폴더에 배치한다. 사용한 OpenSSL DLL은 3.5.9다. 원본 설치본의 `LICENSE`에 MySQL Community 조건, C API 클라이언트의 Universal FOSS Exception 및 별도 구성요소 고지가 있다. [MySQL 8.4 라이선스 안내](https://downloads.mysql.com/docs/licenses/mysqld-8.4-gpl-en.pdf) |
| OpenCV 4.13.0 | 정적 연결. `core`, `imgproc`, `imgcodecs` 사용. Apache 2.0 원문을 `licenses/OpenCV.txt`로 동봉한다. [OpenCV LICENSE](https://github.com/opencv/opencv/blob/4.13.0/LICENSE) |
| libpng 1.6.53 및 zlib 1.3.1 | OpenCV 빌드에 사용한 정적 PNG 의존성이다. OpenCV 설치본의 PNG Reference Library License와 zlib 라이선스를 `licenses/libpng.txt`, `licenses/zlib.txt`로 동봉한다. [OpenCV 4.13.0 포함 소스](https://github.com/opencv/opencv/tree/4.13.0/3rdparty) |

로컬 검수 ZIP에는 위 DLL 설치본이 포함되지 않는다. 저장소 루트에서 WeldScope 자체의 공개 라이선스는 확인되지 않았다. 외부 전달 범위를 결정할 때 프로젝트 라이선스, MySQL 클라이언트와 연결한 실행 파일의 조건, 동봉하는 구성요소의 고지를 함께 확인한다.

## 시연 이미지

작성자는 OPENZDM이고 자료명은 *2D image dataset for the presence or absence of aesthetical defects on EV battery welds.*이다. DOI는 10.5281/zenodo.14025630이다. [Zenodo 원본](https://zenodo.org/records/14025630) · [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)

| 패키지 파일 | 원본 또는 제작 경위 |
|---|---|
| `defect.png` | `train/KO/image_1.png` |
| `samples/normal.png` | `train/OK/image_1.png` |
| `samples/image_10513.png` | `train/OK/image_10513.png` |
| `samples/corrupt.png` | 오류 검사에 쓰는 자체 제작 비이미지 |

패키지 안의 OPENZDM PNG는 원본 바이트를 유지한다. 앱 화면에서는 크기를 조정하고 검사로 계산한 후보를 오버레이한다.
