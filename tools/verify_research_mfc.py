"""Run a WeldScope GUI verifier against a new, private loopback MySQL instance."""

import argparse
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
MYSQL = ROOT / '.tools/mysql-8.4.13-winx64'


def verify(check, executable, output, cohort_options=()):
    executable = executable.resolve(strict=True)
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    data = output / 'mysql-data'
    data.mkdir()
    schema = 'weldscopeaudit'
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = str(probe.getsockname()[1])
    if check not in ('cohort', 'folder'):
        with socket.socket() as device:
            device.bind(('127.0.0.1', 33361))

    server = [MYSQL / 'bin/mysqld.exe', '--no-defaults', f'--basedir={MYSQL}', f'--datadir={data}',
              f'--log-error={output / "mysql.log"}']
    hidden = subprocess.CREATE_NO_WINDOW
    subprocess.run([*server, '--initialize-insecure'], check=True, timeout=180, creationflags=hidden)
    client_args = ['--no-defaults', '--protocol=TCP', '--host=127.0.0.1', '--port=' + port,
                   '--user=root', '--connect-timeout=2']
    admin = [MYSQL / 'bin/mysqladmin.exe', *client_args]
    client = [MYSQL / 'bin/mysql.exe', *client_args, '--batch', '--skip-column-names']
    password = secrets.token_hex(24)
    env = dict(os.environ, WELDSCOPE_MYSQL_HOST='127.0.0.1', WELDSCOPE_MYSQL_PORT=port,
               WELDSCOPE_MYSQL_DATABASE=schema, WELDSCOPE_MYSQL_USER='research_app',
               WELDSCOPE_MYSQL_PASSWORD=password, WELDSCOPE_DEVICE_PORT='33361')
    env.pop('WELDSCOPE_MYSQL_CA_FILE', None)
    with subprocess.Popen([*server, '--bind-address=127.0.0.1', '--port=' + port, '--mysqlx=OFF',
                           '--tls-version='], creationflags=hidden) as process:
        owned = False
        try:
            deadline = time.monotonic() + 45
            while process.poll() is None and time.monotonic() < deadline:
                probe = subprocess.run([*client, '-e', 'SELECT @@datadir'], capture_output=True, text=True)
                if probe.returncode == 0:
                    break
                time.sleep(.3)
            else:
                raise RuntimeError('Private MySQL failed to start; see mysql.log')
            if Path(probe.stdout.strip()).resolve() != data:
                raise RuntimeError('Unexpected MySQL data directory')
            owned = True
            # Grant the same analysis and history permissions as the packaged practice app.
            sql = (f'CREATE DATABASE {schema}; USE {schema};\n'
                   + (ROOT / 'db/s05/schema.sql').read_text(encoding='utf8') + '\n'
                   + (ROOT / 'db/s13/schema.sql').read_text(encoding='utf8') + '\n'
                   + f"CREATE USER 'research_app'@'localhost' IDENTIFIED BY '{password}';\n"
                   + f"GRANT SELECT, INSERT ON {schema}.* TO 'research_app'@'localhost';\n"
                   + f"GRANT UPDATE (state) ON {schema}.inspection_batches TO 'research_app'@'localhost';")
            subprocess.run(client, input=sql.encode('utf8'), check=True)
            subprocess.run([MYSQL / 'bin/mysql.exe', '--no-defaults', '--protocol=TCP', '--host=127.0.0.1',
                            '--port=' + port, '--user=research_app', '--get-server-public-key', schema,
                            '-e', 'SELECT 1'], env=dict(env, MYSQL_PWD=password), check=True,
                           stdout=subprocess.DEVNULL)
            verifier = {'legacy': 'tools.verify_s11_gui', 'portfolio': 'tools.verify_portfolio_mfc', 'folder': 'tools.verify_folder_e2e',
                        'cohort': 'tools.verify_mfc_model_cohort'}[check]
            arguments = [executable, output / 'gui', *cohort_options]
            if check == 'folder':
                arguments = ['verify', '--app', executable, '--output', output / 'gui', '--fixture-image',
                             ROOT / 'artifacts/S13/OPENZDM/train/OK/image_10513.png', '--timeout', '180']
            subprocess.run([sys.executable, '-W', 'error', '-m', verifier, *arguments],
                           env=env, cwd=ROOT, check=True, timeout=1020 if check == 'cohort' else 240)
        finally:
            if owned and process.poll() is None:
                subprocess.run([*admin, 'shutdown'], timeout=35)
            if process.poll() is None:
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.terminate()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('check', choices=('legacy', 'portfolio', 'folder', 'cohort'))
    parser.add_argument('executable', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--cohort', nargs=3, metavar=('MANIFEST', 'REFERENCE_CSV', 'VERSION'))
    args = parser.parse_args()
    if (args.check == 'cohort') != bool(args.cohort):
        parser.error('The cohort check requires --cohort MANIFEST REFERENCE_CSV VERSION')
    verify(args.check, args.executable, args.output, args.cohort or ())
