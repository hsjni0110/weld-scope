"""Run separately from CTest: exercises the real PowerShell workflow (which runs CTest)."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

root = Path(__file__).resolve().parents[1]
checks = []
with TemporaryDirectory(prefix='workflow-check-', dir=root / 'artifacts/S04') as directory:
    work = Path(directory)
    inputs = work / 'inputs'
    shutil.copytree(root / 'artifacts/S04/sample-inputs', inputs)
    manifest = json.loads((inputs / 'manifest.json').read_text())
    (inputs / 'images' / manifest['rows'][0]['path']).write_bytes(b'corrupted local copy')
    def demo(*arguments):
        return subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                               str(root / 'tools/demo-s04.ps1'), *map(str, arguments)],
                              cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    error_output = work / 'with-error'
    result = demo('-Output', error_output, '-InputDirectory', inputs)
    assert result.returncode == 2, result.stdout.decode(errors='replace')
    report = json.loads((error_output / 'report/development-summary.json').read_text())
    assert report['attempted'] == 16 and report['decisions']['ERROR'] == 1
    checks.append('Completed ERROR run still creates its report and returns 2')
    wrong_output = work / 'wrong-full'
    result = demo('-Output', wrong_output, '-InputDirectory', root / 'artifacts/S04/sample-inputs', '-FullTrain')
    assert result.returncode != 0 and not wrong_output.exists(), result.stdout.decode(errors='replace')
    checks.append('FullTrain rejects the 16-image sample cache before inspection')
    single_output = work / 'single'
    executable = root / 'artifacts/S04/build/Release/weldscope_inspect.exe'
    subprocess.run([str(executable), '--config', str(root / 'config/baseline.json'), '--image',
                    str(root / 'src/WeldScope.App/res/samples/normal.png'), '--output', str(single_output)], check=True)
    subprocess.run([sys.executable, '-W', 'error', 'tools/report_s04.py', '--run', str(single_output)], cwd=root, check=True)
    report = json.loads((single_output / 'report/development-summary.json').read_text())
    assert report['scope'] == 'unlabelled-single-image' and report['openzdm_attribution_verified'] is False
    checks.append('Single-file report preserves unspecified provenance without dataset attribution')
(root / 'artifacts/S04/workflow-checks.json').write_text(json.dumps({'checks': checks}, indent=2) + '\n', encoding='utf-8')
print('PASS:', '; '.join(checks))
