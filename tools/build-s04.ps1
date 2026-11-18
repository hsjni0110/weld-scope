param([string]$Python)
$Configuration = 'Release'
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot/python-s04.ps1"
$python = Resolve-S04Python -RepositoryRoot $repoRoot -Python $Python
$cmake = 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$build = Join-Path $repoRoot 'artifacts/S04/build'
$opencv = Join-Path $repoRoot '.tools/opencv/install-arm64'
if (-not (Test-Path -LiteralPath $opencv)) { throw 'Run tools/setup-opencv-s04.ps1 first' }
& $cmake -S $repoRoot -B $build -G 'Visual Studio 18 2026' -A ARM64 "-DOpenCV_DIR=$opencv" -DOpenCV_STATIC=ON "-DPython3_EXECUTABLE=$python"
if ($LASTEXITCODE -ne 0) { throw 'S04 configuration failed' }
& $cmake --build $build --config $Configuration --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'S04 build failed' }
& (Join-Path (Split-Path $cmake -Parent) 'ctest.exe') --test-dir $build -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'S04 checks failed' }
