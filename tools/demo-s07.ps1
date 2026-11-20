param(
    [string]$Output = 'artifacts/S07/demo',
    [switch]$FullTrain,
    [switch]$ReverseOrder,
    [string]$Reference,
    [string]$InputDirectory,
    [string]$Python
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot/python-s04.ps1"
$python = Resolve-S04Python -RepositoryRoot $repoRoot -Python $Python
Push-Location $repoRoot
try {
    if (Test-Path -LiteralPath $Output) { throw 'Choose a new output directory' }
    if (-not (Test-Path '.tools/opencv/install-arm64/OpenCVConfig.cmake')) { & "$PSScriptRoot/setup-opencv-s04.ps1" }
    & "$PSScriptRoot/build-s04.ps1" -Python $python

    if (-not $InputDirectory) {
        $InputDirectory = if ($FullTrain) { 'artifacts/S04/inputs' } else { 'artifacts/S04/sample-inputs' }
    }
    $manifest = Join-Path $InputDirectory 'manifest.json'
    $preparation = @('-m', 'tools.prepare_s04_inputs', '--output', $InputDirectory)
    if (-not $FullTrain) { $preparation += '--samples-only' }
    if (-not (Test-Path -LiteralPath $manifest)) {
        & $python @preparation
        if ($LASTEXITCODE -ne 0) { throw 'Input preparation failed' }
    }
    & $python @preparation --verify-only
    if ($LASTEXITCODE -ne 0) { throw 'Cached input selection mismatch' }

    $executable = './artifacts/S04/build/Release/weldscope_inspect.exe'
    $modes = @('baseline', 'fast', 'precise')
    if ($ReverseOrder) { [Array]::Reverse($modes) }
    & $python -m tools.report_s07 --run $Output --manifest $manifest --record-context --executable $executable --order @modes
    if ($LASTEXITCODE -ne 0) { throw 'Could not record inspection run context' }
    $pendingContext = Join-Path $Output 'run-context.pending.json'
    $context = Get-Content -LiteralPath $pendingContext -Raw | ConvertFrom-Json
    $inspectionExit = 0
    foreach ($mode in $modes) {
        $config = if ($mode -eq 'baseline') { 'config/baseline.json' } else { 'config/local-contrast.json' }
        & $executable --algorithm $mode --manifest $manifest --config $config --output (Join-Path $Output $mode)
        if ($LASTEXITCODE -notin @(0, 2)) { throw "$mode failed before completing its result records" }
        if ($LASTEXITCODE -eq 2) { $inspectionExit = 2 }
    }

    if ((Get-FileHash -LiteralPath $executable).Hash -ne $context.executable_sha256 -or
        (Get-FileHash -LiteralPath $manifest).Hash -ne $context.manifest_sha256) {
        throw 'The executable or manifest changed during inspection'
    }
    Rename-Item -LiteralPath $pendingContext -NewName 'run-context.json'
    $report = @('-m', 'tools.report_s07', '--run', $Output, '--manifest', $manifest)
    if ($Reference) { $report += @('--reference', $Reference) }
    & $python @report
    if ($LASTEXITCODE -ne 0) { throw 'Comparison report failed' }
    exit $inspectionExit
} finally { Pop-Location }
