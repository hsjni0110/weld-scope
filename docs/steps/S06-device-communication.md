# S06 가상 장비 통신

구현과 루프백 검증을 마쳤다. 사용자 결과 검수 대기 상태다. S01을 선행 조건으로 하며 기준 `main`은 S05가 병합된 `f225e6a`다. [로드맵](../ROADMAP.md) · [PRD F08](https://chatgpt.com/space/page_b35b9c74caac81918226ccdc9e38769e)

## 목표와 범위

가상 장비가 TCP로 등록된 입력과 설정을 지정하면 검사 콘솔이 샘플 결과를 응답한다. 한 번의 시연에서 요청·응답, 타임아웃, 재연결, 재전송과 충돌을 확인한다. Windows ARM64 C++17 콘솔과 Python 3 표준 라이브러리 스크립트로 구성한다. 실제 검사와 이력 저장을 연결하는 단계는 S10이다. [ADR 0006](../adr/0006-virtual-device-protocol.md)

고정 입력 `weld-01`은 S05 샘플의 공개 OPENZDM 원본 `train/KO/image_1.png`를 가리킨다. 설정 `baseline-v1`은 고정 시연 설정을 식별한다. 응답은 고정 판정 `FAIL`을 사용하며 콘솔에는 `mode=sample`을 표시한다. 입력과 설정은 ID로 전달한다.

## 메시지 계약

프레임은 ASCII 필드를 TAB으로 구분하고 LF로 끝낸다. 아래의 `\t`와 `\n`은 실제 TAB과 LF다. 프레임 본문은 최대 256바이트다. 모든 ID는 영문자 또는 숫자로 시작하는 1~64바이트로, 뒤에는 영문자·숫자·밑줄·하이픈·마침표를 허용한다. ID는 대소문자를 구분한다.

```text
INSPECT\t<request_id>\t<source_id>\t<config_id>\n
RESULT\t<request_id>\t<inspection_id>\tFAIL\n
ERROR\t<request_id 또는 ->\t<code>\n
```

| 조건 | 동작 |
|---|---|
| 처음 받은 유효 요청 | 새 검사 ID와 샘플 결과를 응답 전에 보관한다. |
| 동일 요청 ID와 동일 입력·설정 | 같은 검사 ID와 동일한 응답 프레임을 반환한다. 연결을 다시 열어도 프로세스가 같으면 적용한다. |
| 동일 요청 ID와 다른 입력 또는 설정 | `REQUEST_CONFLICT`를 반환한다. 등록 여부와 한도를 검사하기 전에 기존 요청과 비교한다. |
| 등록되지 않은 입력 또는 설정 | 새 요청에서 `UNKNOWN_SOURCE` 또는 `UNKNOWN_CONFIG`를 반환한다. 둘 다 미등록이면 입력 오류를 먼저 반환한다. |
| 보관 한도 256건 | 새 유효 요청은 `CAPACITY`다. 기존 요청의 응답은 계속 재전송할 수 있다. |
| 잘못된 명령·필드 수·문자·상한 초과 | `ERROR\t-\tBAD_FRAME\n`을 보낼 수 있으면 보내고 해당 연결을 닫는다. |
| 프레임 수신 또는 응답 전송 시간 초과 | 해당 연결을 닫고 서버 로그에 `timeout`과 수신·전송 구분을 남긴다. 프레임 중간 EOF는 `incomplete_frame`으로 기록한다. |

첫 프레임 수신 마감은 서버가 연결을 수락한 시점부터 2초다. 각 응답을 보낸 뒤에는 다음 프레임을 위한 2초 수신 마감을 시작한다. 개별 바이트를 받더라도 해당 프레임 마감은 연장하지 않는다. 응답 전송도 프레임당 최대 2초를 사용한다. 마감과 전송 실패는 연결 상태로 다룬다. 이미 보관한 샘플 결과는 그대로 유지한다.

## 빌드와 재현

저장소 루트에서 실행한다. `ProtocolTests.cpp`는 TCP 수신 호출의 경계와 독립적으로 분할·병합, 256/257바이트 경계와 필드 문법을 검증한다. 시연 스크립트는 `weldscope_device.exe --port 0`을 띄우고 `mode=sample`과 OS가 할당한 포트를 읽는다. 마지막 줄 `S06 checks passed`와 종료 코드 0을 확인한다. `.tools/python/python.exe` 대신 설치된 Python 3으로 실행할 수 있다.

```powershell
cmake -S src/WeldScope.Device -B artifacts/S06/build -G 'Visual Studio 18 2026' -A ARM64
cmake --build artifacts/S06/build --config Release
ctest --test-dir artifacts/S06/build -C Release --output-on-failure
.tools/python/python.exe tools/demo-s06.py artifacts/S06/build/Release/weldscope_device.exe
```

## 실제 실행 결과

Windows ARM VM의 Visual Studio 2026 도구로 ARM64 Release를 빌드했다. 실행 파일 헤더는 `AA64 machine (ARM64)`였다. CTest 1개와 실제 루프백 시연이 통과했다.

| 시나리오 | 관찰 결과 |
|---|---|
| 정상 요청과 분할·병합 | 마지막 LF를 보내기 전에는 응답이 없었다. 완성 뒤 `RESULT\treq-001\ti-1\tFAIL`을 받았다. 한 번에 보낸 다음 두 요청은 순서대로 응답했고 검사 ID가 각각 달랐다. |
| 재연결과 충돌 | 동일 요청의 재전송 응답이 최초와 바이트 단위로 일치했다. 입력·설정을 각각 바꾸면 `REQUEST_CONFLICT`가 왔다. 새 요청의 미등록 입력과 설정에는 각각 `UNKNOWN_SOURCE`, `UNKNOWN_CONFIG`가 왔다. |
| 응답을 읽기 전에 연결 종료 | 가상 장비가 결과 보관 로그를 확인한 뒤 응답을 읽지 않고 연결을 닫았다. 재연결 후 같은 검사 ID `i-4`를 받았고 생성은 한 번만 기록됐다. |
| 수신·송신 타임아웃 | 요청의 끝부분을 조금씩 보내도 약 2.01초 뒤 연결이 닫혔다. 응답을 읽지 않는 클라이언트로 소켓 송신 대기를 유도하자 `event=timeout operation=send`가 기록됐고, 재연결에서 보관한 검사 ID를 받았다. |
| 중간 EOF·초과·잘못된 프레임 | 중간 EOF는 `incomplete_frame`으로 기록됐고, 초과와 필드 오류에는 `BAD_FRAME`을 받았다. 오류 요청으로 검사가 생성되지 않았고 정상 재요청은 성공했다. 완전한 요청 직후 쓰기 방향만 닫은 경우에는 응답이 도착했다. |
| 보관 한도 | 별도 서버에서 256건을 처리했다. 257번째 새 요청은 `CAPACITY`였고, 처음 요청의 재전송은 기존 응답과 일치했다. |

검사 ID와 재전송 보관 기간은 S06 콘솔 프로세스 수명이다. 검증 환경은 Windows ARM64 루프백이며, 실제 장비와 S10 통합은 후속 단계 대상이다.
