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

DB 이름은 `weldscope_s05`, 콘솔 계정은 `weldscope_app`이다. 비밀번호는 Mac의 `.secrets/mysql-app-password`에 있다. Mac에서 관리자 계정으로 이력 스키마를 적용한다. 새 볼륨과 기존 볼륨에 같은 명령을 사용한다. 재실행하면 기존 행이 보존된다.

```zsh
docker compose exec -T mysql sh -c 'MYSQL_PWD="$(cat /run/secrets/mysql_root_password)" mysql -u root weldscope_s05' < db/s05/schema.sql
```

Windows에서 [S05 빌드와 시연](../../docs/steps/S05-storage-history.md)을 실행한다.

## Windows VM에서 서버 인증

원격 시연에는 Mac MySQL 서버 이름이 DNS SAN에 포함된 서버 인증서가 필요하다. 예시 서버 이름은 `weldscope-mysql.test`다. 준비한 CA 인증서, 서버 인증서, 서버 개인 키를 Mac의 `.secrets/mysql-tls` 아래 `ca.pem`, `server-cert.pem`, `server-key.pem`으로 둔다. CA 개인 키는 이 디렉터리 밖에 보관한다. Mac에서는 상위 `.secrets`를 소유자만 접근할 수 있게 유지하고, Docker 컨테이너의 MySQL 사용자에게 파일 읽기 권한을 준다. 예시 설정은 기존 MySQL 볼륨을 사용해 컨테이너를 재구성한다.

```zsh
chmod 755 .secrets/mysql-tls
chmod 644 .secrets/mysql-tls/ca.pem .secrets/mysql-tls/server-cert.pem .secrets/mysql-tls/server-key.pem
docker compose -f compose.yaml -f db/s05/compose.tls.yaml up -d --wait --wait-timeout 120
```

Windows VM에서 서버 이름을 VM이 접속하는 Mac 주소로 해석하도록 DNS 또는 hosts 파일을 설정한다. Emulated VLAN의 기본 예시는 `10.0.2.2 weldscope-mysql.test`다. 신뢰할 경로로 CA 인증서 `ca.pem`을 Windows에 복사한다. Windows 접속 변수에는 인증서의 서버 이름과 복사한 CA 경로를 지정한다. 서버 개인 키는 Mac에 둔다.

중지와 재시작은 `docker compose stop mysql`과 `docker compose start mysql`을 사용한다. 볼륨에는 데이터와 초기화된 계정이 남는다. 비밀번호 파일을 다시 만들어도 기존 DB 계정 비밀번호는 바뀌지 않는다.

이전 `weldscope-s05` 프로젝트를 실행했다면 `docker compose -p weldscope-s05 stop mysql`로 이전 컨테이너를 중지해 포트를 비운다. 새 프로젝트의 기본 볼륨은 `weldscope_mysql_data`다. 이전 볼륨의 데이터를 이어 쓰려면 루트의 로컬 `.env`에 `WELDSCOPE_MYSQL_VOLUME=weldscope-s05_mysql_data`를 설정하고 기존 비밀번호 파일로 실행한다.
