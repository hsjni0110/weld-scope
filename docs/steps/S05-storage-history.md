# S05 검사 이력 저장과 조회

독립 콘솔 구현과 MySQL 검증을 마쳤으며 main에 병합됐다.

## 왜

검사가 끝난 뒤 같은 요청이 다시 오거나 저장 중 연결이 끊겨도 검사 이력이 한 번만 기록되어야 한다. S05는 고정 샘플로 저장·조회·재전송·복구를 실제 MySQL에서 확인한다. [PRD F07](https://chatgpt.com/space/page_b35b9c74caac81918226ccdc9e38769e)과 [로드맵](../ROADMAP.md)의 단계 기준을 따른다. S01이 최소 선행 조건이며 이번 계획의 기준 커밋은 S04가 병합된 `8bc0b75`다.

## 어떻게

작은 C++17 콘솔 하나와 MySQL 저장 모듈 하나를 둔다. 콘솔은 고정 샘플로 모듈을 호출하고, 모듈은 파라미터를 바인딩한 SQL과 트랜잭션을 사용한다. 검사 결과와 분석 시도는 두 테이블에 보관한다. 자세한 저장·재시도 계약은 [ADR 0005 제안](../adr/0005-inspection-history.md)을 따른다. 기존 S04 콘솔과 MFC의 빌드에는 새 DB 의존성을 추가하지 않는다. MFC·TCP 연결과 운영 중 새 입력 중단은 S10 범위다.

## Windows 빌드와 시연

Windows ARM VM에서 Visual Studio 2026의 x64 C++ 도구와 MySQL Community Server 8.4.13 Windows x86-64 ZIP을 사용했다. 아래 명령은 저장소 루트에서 실행한다. ZIP을 `.tools/mysql-8.4.13-winx64`에 푼다. CMake 빌드 시 MySQL과 OpenSSL DLL을 실행 파일 옆에 복사한다.

```powershell
$mysql = (Resolve-Path .tools/mysql-8.4.13-winx64).Path
cmake -S src/WeldScope.Storage -B artifacts/S05/build -G 'Visual Studio 18 2026' -A x64 "-DMYSQL_ROOT=$mysql"
cmake --build artifacts/S05/build --config Release
```

Mac의 [DB 사전 설정](../../db/s05/README.md)에 따라 스키마를 적용하고 서버 TLS 인증서를 준비한다. Windows에서 인증서의 서버 이름과 신뢰할 CA 파일을 지정한다.

```powershell
$env:WELDSCOPE_MYSQL_HOST = 'weldscope-mysql.test'
$env:WELDSCOPE_MYSQL_PORT = '3309'
$env:WELDSCOPE_MYSQL_USER = 'weldscope_app'
$env:WELDSCOPE_MYSQL_DATABASE = 'weldscope_s05'
$env:WELDSCOPE_MYSQL_CA_FILE = (Resolve-Path .secrets/mysql-ca.pem).Path
$appPassword = Read-Host 'MySQL app password' -AsSecureString
$env:WELDSCOPE_MYSQL_PASSWORD = [System.Net.NetworkCredential]::new('', $appPassword).Password
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/demo-s05.ps1
```

마지막 줄 `S05 checks passed`와 그 앞의 저장·조회·재전송·충돌·롤백 결과를 확인한다. 시연은 매번 새 ID를 사용한다. 샘플은 OPENZDM의 S04 실행 결과 일부를 고정한 값으로, 영상 처리 없이 저장 계약을 검증한다.

S10 이전 스키마로 이미 준비한 개발 DB에서 현재 저장 모듈을 실행하려면 [S05 → S10 마이그레이션](../../db/s10/migrate-from-s05.sql)을 한 번 적용한다. 새 DB는 현재 `db/s05/schema.sql`로 초기화한다.

## 별도 인스턴스에서 동시 저장과 연결 단절 재현

전용 Windows MySQL 인스턴스의 TCP 포트는 `33359`로 사용했다. 다음 명령은 PowerShell 창 하나에서 실행한다. `mysql-data`를 처음 만들 때만 초기화한다. 테스트 프록시는 평문 MySQL 패킷을 전달하므로 테스트 인스턴스를 루프백에 바인딩하고 TLS를 끈다. [MySQL 서버의 tls_version 설정](https://dev.mysql.com/doc/refman/8.4/en/server-system-variables.html#sysvar_tls_version)

```powershell
$mysql = (Resolve-Path .tools/mysql-8.4.13-winx64).Path
$data = Join-Path $PWD 'artifacts/S05/mysql-data'
& "$mysql/bin/mysqld.exe" --no-defaults --initialize-insecure "--basedir=$mysql" "--datadir=$data" --console
& "$mysql/bin/mysqld.exe" --no-defaults "--basedir=$mysql" "--datadir=$data" --bind-address=127.0.0.1 --port=33359 --mysqlx=OFF --tls-version= --console
```

서버의 `ready for connections` 이후 다른 PowerShell 창에서 테스트 계정과 스키마를 준비한다. 내장 Python 대신 설치된 Python 3을 사용할 수 있다.

```powershell
$mysql = (Resolve-Path .tools/mysql-8.4.13-winx64).Path
$admin = @('--no-defaults', '-h', '127.0.0.1', '-P', '33359', '-u', 'root')
$env:WELDSCOPE_MYSQL_HOST = '127.0.0.1'
$env:WELDSCOPE_MYSQL_PORT = '33359'
$env:WELDSCOPE_MYSQL_USER = 'weldscope_app'
$env:WELDSCOPE_MYSQL_DATABASE = 'weldscope_s05'
Remove-Item Env:WELDSCOPE_MYSQL_CA_FILE -ErrorAction SilentlyContinue
$env:WELDSCOPE_MYSQL_PASSWORD = [guid]::NewGuid().ToString('N')
& "$mysql/bin/mysql.exe" @admin -e 'CREATE DATABASE IF NOT EXISTS weldscope_s05;'
Get-Content db/s05/schema.sql -Raw | & "$mysql/bin/mysql.exe" @admin weldscope_s05
"CREATE USER IF NOT EXISTS 'weldscope_app'@'%' IDENTIFIED BY '$env:WELDSCOPE_MYSQL_PASSWORD'; ALTER USER 'weldscope_app'@'%' IDENTIFIED BY '$env:WELDSCOPE_MYSQL_PASSWORD';" | & "$mysql/bin/mysql.exe" @admin
Get-Content db/s05/01-app-permissions.sql -Raw | & "$mysql/bin/mysql.exe" @admin
$env:MYSQL_PWD = $env:WELDSCOPE_MYSQL_PASSWORD
& "$mysql/bin/mysql.exe" --no-defaults --get-server-public-key -h 127.0.0.1 -P 33359 -u weldscope_app weldscope_s05 -e 'SELECT 1'
Remove-Item Env:MYSQL_PWD
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/demo-s05.ps1
.tools/python/python.exe tools/verify_s05_integration.py artifacts/S05/build/Release/weldscope_history.exe "$mysql/bin/mysql.exe"
& "$mysql/bin/mysqladmin.exe" @admin shutdown
```

첫 앱 계정 로그인은 서버 공개 키를 사용해 MySQL 인증 캐시를 채운다. 이후 시연 콘솔이 같은 계정으로 접속한다. 통합 검사는 루프백 테스트 인스턴스 관리자 계정으로 테이블 잠금을 잡아 두 저장 요청이 동시에 대기하는지 확인한다. 이어 트랜잭션 직전과 서버 커밋 확인 직후에 각각 연결을 끊고, 서버에 직접 재접속해 조회 및 재전송한다. 두 스크립트의 성공 문구는 각각 `S05 checks passed`와 `S05 database integration checks passed`다.

## 관찰 결과

2026-11-19에 Windows ARM VM의 x64 Release 콘솔과 로컬 MySQL 8.4.13 전용 인스턴스에서 확인했다. `SELECT VERSION()` 결과는 `8.4.13`이었고 앱 계정으로 시연을 실행했다. 아래 행 수는 기존 데이터가 있는 상태에서 스키마 SQL을 다시 실행한 뒤 DB에서 조회했다.

| 수락 기준 | 관찰 결과 | 검사 / 시도 행 |
|---|---|---|
| 저장, 별도 프로세스 조회, 동일 내용 재전송 | `stored → found → replay`. 원본 경로·해시·판정·점수·시간을 조회했고, 재전송 전후 생성 시각과 결과가 일치했다. | 1 / 1 |
| 동시 재전송과 시도 순서 | 두 DB 세션이 같은 테이블 잠금을 기다린 것을 확인 후 해제했다. 한 호출은 `stored`, 다른 호출은 `replay`였다. 두 시도를 순번대로 읽었고 2번 점수는 NULL이었다. | 1 / 2 |
| 두 ID 충돌과 내용 변경 | 다른 검사 ID, 다른 요청 ID, 변경된 점수를 각각 `conflict`로 반환했다. 최초 조회와 최종 조회가 일치했다. | 1 / 1 |
| 시도 삽입 실패 | 두 번째 시도에서 음수 소요 시간이 MySQL 제약 오류 3819를 일으켰다. 이후 검사 조회는 `not_found`였다. | 0 / 0 |
| 길이 초과 문자열 | `input_id`와 시도의 `stage`에 각각 길이 제한을 넘는 공백을 넣었다. 수정 전에는 `input_id`가 잘린 채 저장됐다. 수정 후 두 경우 모두 쓰기 경고로 롤백됐으며 검사 조회는 `not_found`였다. | 0 / 0 |
| 연결 단절 후 재전송 | `failed`와 MySQL 오류 2013이 표시됐다. 판정은 `FAIL`이었다. 최초 조회는 `not_found`, 재전송은 `stored`, 다음 재전송은 `replay`였다. | 1 / 1 |
| 서버 커밋 후 응답 유실 | 서버 커밋 성공 응답을 프록시에서 받은 뒤 클라이언트 연결을 끊었다. CLI에 `commit_unknown`과 MySQL 오류 2013이 표시됐다. 직접 조회는 `found`, 재전송은 `replay`였고 조회 결과가 일치했다. | 1 / 1 |

같은 프로세스의 8개 스레드에서 동시에 첫 `SELECT VERSION()`을 실행해 모두 성공했다. 인증서를 설정한 로컬 MySQL에서 신뢰할 CA와 올바른 서버 이름으로 시연을 실행했다. 신뢰하지 않는 CA와 이름이 다른 인증서는 MySQL 오류 2026으로 거부됐고, CA를 지정하지 않은 원격 접속도 거부됐다.

사용자 검수 항목은 이력 시연 결과와 [ADR 0005](../adr/0005-inspection-history.md)의 저장 계약이다. Mac Docker의 Compose TLS 구성은 병합 구문을 확인했다. 실제 Mac 앱 계정 연결과 ARM64 네이티브 MySQL 클라이언트는 미검증이다.
