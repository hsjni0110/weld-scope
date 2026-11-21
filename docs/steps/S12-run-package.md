# WeldScope x64 검수 패키지 실행

Windows 11에서 ZIP을 새 폴더에 푼다. x64 앱과 CLI를 실행하려면 [Microsoft VC++ x64 런타임](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)을 설치한다. 파일 목록과 해시는 `SHA256SUMS.txt`에서 확인한다. OpenCV는 실행 파일에 연결돼 있다.

## MySQL 연결

전용 Windows 루프백 테스트 서버를 준비한다. [MySQL Community 8.4.13](https://dev.mysql.com/downloads/mysql/8.4.html)의 `lib/libmysql.dll`, `bin/libssl-3-x64.dll`, `bin/libcrypto-3-x64.dll`을 압축을 푼 앱 폴더로 복사한다. 서버 관리자 권한으로 테스트 데이터베이스를 만들고 패키지의 `db/schema.sql`을 적용한다. 앱 전용 계정에는 그 스키마의 `SELECT, INSERT` 권한을 준다.

```powershell
$env:WELDSCOPE_MYSQL_HOST = '127.0.0.1'
$env:WELDSCOPE_MYSQL_PORT = '33365'       # 테스트 서버의 실제 포트
$env:WELDSCOPE_MYSQL_DATABASE = 'weldscope_s12'
$env:WELDSCOPE_MYSQL_USER = 'weldscope_app'
$pw = Read-Host 'MySQL app password' -AsSecureString
$env:WELDSCOPE_MYSQL_PASSWORD = [Net.NetworkCredential]::new('', $pw).Password
Remove-Item Env:WELDSCOPE_MYSQL_CA_FILE -ErrorAction SilentlyContinue
```

TLS를 끈 전용 서버에서는 서버 시작과 재시작 뒤 이 명령으로 앱 계정의 첫 접속을 확인한다. 설치 위치에 맞게 `mysql.exe` 경로를 바꾸고 `-p` 프롬프트에는 위와 같은 비밀번호를 입력한다. 서버가 종료되면 MySQL의 인증 캐시가 비워진다. [MySQL 인증 캐시](https://dev.mysql.com/doc/refman/8.4/en/caching-sha2-pluggable-authentication.html)

```powershell
& 'C:\mysql\bin\mysql.exe' --no-defaults --protocol=TCP --get-server-public-key -h 127.0.0.1 -P $env:WELDSCOPE_MYSQL_PORT -u $env:WELDSCOPE_MYSQL_USER -p $env:WELDSCOPE_MYSQL_DATABASE -e 'SELECT 1'
```

접속이 성공한 뒤 앱을 실행한다.

```powershell
.\WeldScope.App.exe
```

패키지의 샘플은 시연용 이미지다.

## 파일 검사와 이력

1. `DB 연결` 상태에서 `samples/normal.png`를 선택하고 검사한다. 화면에서 판정, 원본과 후보, 분석 단계, 저장 완료를 확인한다.
2. `samples/image_10513.png`를 열어 단계 이력을 확인한다. S12 당시의 **선택적 검사**에서는 시간이 남으면 이 사진이 정밀 단계로 들어갔다. 현재 기본 모델 **연속 보간**에서 이 사진은 빠른 통과 구간이며 분석 예산을 넘기면 보류한다.
3. `samples/corrupt.png`로 처리 오류를 확인한 뒤 정상 입력으로 복귀한다. 전용 DB가 내려간 상태에서 앱을 열면 시작이 제한된다. DB를 다시 시작한다. TLS를 끈 서버에서는 시작할 때 사용한 `SELECT 1` 접속 명령을 다시 실행한다. 접속이 성공하면 앱에서 `재연결`을 누른다.

## TCP 요청과 재전송

앱이 열린 상태에서 다른 PowerShell 창에서 실행한다. `STATUS`가 오면 같은 ID로 다시 호출해 `RESULT`를 확인한다. 앱을 다시 시작한 뒤에도 같은 호출의 검사 ID와 판정이 일치해야 한다.

```powershell
function Send-WeldScopeRequest([string]$RequestId) {
    $client = [Net.Sockets.TcpClient]::new('127.0.0.1', 33361)
    try {
        $client.ReceiveTimeout = 5000
        $client.SendTimeout = 5000
        $stream = $client.GetStream()
        $frame = [Text.Encoding]::ASCII.GetBytes("INSPECT`t$RequestId`tweld-01`tselective-v1`n")
        $stream.Write($frame, 0, $frame.Length)
        [IO.StreamReader]::new($stream).ReadLine()
    } finally {
        $client.Dispose()
    }
}
$id = 's12-' + [guid]::NewGuid().ToString('N')
Send-WeldScopeRequest $id
```

## DB 없이 CLI 검사

```powershell
.\weldscope_inspect.exe --algorithm selective --config selective.json --image samples/image_10513.png --output cli-result
```

`cli-result/results.csv`, 요약과 후보 이미지를 확인한다. 다음 실행에서는 새 출력 폴더를 지정한다. 라이선스와 영상 원본은 `THIRD-PARTY-NOTICES.md`를 참조한다.
