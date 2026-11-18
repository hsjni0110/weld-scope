# S05 MySQL 사전 설정

Mac에서 Docker Desktop을 시작한다. 저장소 루트에서 다음 명령을 실행한다. 비밀번호 파일이 이미 있으면 그대로 사용한다.

```zsh
mkdir -p -m 700 .secrets
chmod 700 .secrets
(
  umask 022
  for account in root app; do
    file=".secrets/mysql-${account}-password"
    if [ ! -f "$file" ]; then openssl rand -hex 24 > "$file"; fi
    chmod 644 "$file"
  done
)
docker compose up -d --wait --wait-timeout 120
docker compose ps
```

Mac에서는 소유자만 `.secrets` 디렉터리에 접근할 수 있다. 개별 파일은 컨테이너의 MySQL 사용자도 마운트 경로에서 읽을 수 있게 한다. 기본 포트가 사용 중이면 `WELDSCOPE_MYSQL_PORT=3310 docker compose up -d --wait --wait-timeout 120`처럼 빈 포트를 지정한다. 이 구성은 전용 볼륨을 사용하므로 수동 컨테이너의 DB와 별개다.

앱 계정과 실제 서버 버전을 확인한다.

```zsh
docker compose exec mysql sh -c 'MYSQL_PWD="$(cat /run/secrets/mysql_app_password)" mysql --protocol=TCP -h 127.0.0.1 -u weldscope_app -D weldscope_s05 -e "SELECT DATABASE(), VERSION(); SHOW GRANTS;"'
```

초기화는 비어 있는 전용 볼륨에서 한 번 실행된다. 기본 게시 주소는 Mac의 `127.0.0.1:3309`다. UTM Emulated VLAN을 쓰면 Windows PowerShell에서 아래처럼 확인한다. UTM의 Host Address나 게시 포트를 변경했다면 실제 값을 사용한다.

```powershell
Test-NetConnection -ComputerName 10.0.2.2 -Port 3309
```

Shared Network에서는 Mac의 VM 접속용 인터페이스 주소를 확인한 뒤 `WELDSCOPE_MYSQL_BIND_IP`에 지정하고 Compose를 다시 실행한다. Windows에서도 그 주소로 접속한다. Mac의 Wi-Fi 주소를 고정 설정에 넣지 않는다.

DB 이름은 `weldscope_s05`, 콘솔 계정은 `weldscope_app`이다. 비밀번호는 Mac의 `.secrets/mysql-app-password`에 있다. Windows 포트 확인 이후에는 S05 콘솔에서 실제 로그인과 `SELECT VERSION()`을 확인한다. 검사 이력 테이블은 S05 구현에서 추가한다.

중지와 재시작은 `docker compose stop mysql`과 `docker compose start mysql`을 사용한다. 볼륨에는 데이터와 초기화된 계정이 남는다. 비밀번호 파일을 다시 만들어도 기존 DB 계정 비밀번호는 바뀌지 않는다.
