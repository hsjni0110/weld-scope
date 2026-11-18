param([string]$Output = 'artifacts/S04/demo', [switch]$FullTrain, [string]$InputDirectory = '', [string]$Python)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot/python-s04.ps1"
$python = Resolve-S04Python -RepositoryRoot $repoRoot -Python $Python
Push-Location $repoRoot
try {
    if (Test-Path -LiteralPath $Output) { throw 'Choose a new output directory' }
    if (-not (Test-Path '.tools/opencv/install-arm64/OpenCVConfig.cmake')) { & "$PSScriptRoot/setup-opencv-s04.ps1" }
    & "$PSScriptRoot/build-s04.ps1" -Python $python
    if (-not $InputDirectory) { $InputDirectory = if ($FullTrain) { 'artifacts/S04/inputs' } else { 'artifacts/S04/sample-inputs' } }
    $preparation = @('-m', 'tools.prepare_s04_inputs', '--output', $InputDirectory)
    if (-not $FullTrain) { $preparation += '--samples-only' }
    if (-not (Test-Path "$InputDirectory/manifest.json")) {
        & $python @preparation
        if ($LASTEXITCODE -ne 0) { throw 'Input preparation failed' }
    }
    & $python @preparation --verify-only
    if ($LASTEXITCODE -ne 0) { throw 'Cached input selection mismatch' }
    & './artifacts/S04/build/Release/weldscope_inspect.exe' --manifest "$InputDirectory/manifest.json" --config config/baseline.json --output $Output
    $inspectionExit = $LASTEXITCODE
    if ($inspectionExit -notin @(0, 2)) { throw 'Inspection failed before completing its result records' }
    & $python tools/report_s04.py --run $Output
    if ($LASTEXITCODE -ne 0) { throw 'Report failed' }
    exit $inspectionExit
} finally { Pop-Location }
