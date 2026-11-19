param([string]$Executable = 'artifacts/S05/build/Release/weldscope_history.exe')

$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path

function Invoke-History([int]$ExpectedExit, [string]$ExpectedLine, [string[]]$Arguments, [string]$ExpectedError = '') {
    $ErrorActionPreference = 'Continue'
    $output = @(& $executablePath @Arguments 2>&1 | ForEach-Object { [string]$_ })
    if ($LASTEXITCODE -ne $ExpectedExit -or $output -notcontains $ExpectedLine -or
        ($ExpectedError -and -not ($output | Where-Object { $_.StartsWith($ExpectedError, [StringComparison]::Ordinal) }))) {
        throw "Unexpected result for $($Arguments -join ' '): $output"
    }
    $output
}

$run = 's05-' + [guid]::NewGuid().ToString('N')
$inspection = "$run-i"
$request = "$run-r"
$saveArguments = @('save', $inspection, $request)
$getArguments = @('get', $inspection)

$version = & $executablePath version
if ($LASTEXITCODE -ne 0 -or $version -notmatch '^server_version=8\.4\.') { throw 'MySQL 8.4 connection check failed' }
$version
Invoke-History 0 'status=stored' $saveArguments
$stored = @(Invoke-History 0 'status=found' $getArguments)
$stored
foreach ($field in @("inspection_id=$inspection", "request_id=$request", 'decision=FAIL',
    'source_path=train/KO/image_1.png',
    'source_sha256=4a08fa2c6274e603f18358134092e8f697cc81f78a36f472577bca81bc0de3f3',
    'attempt_count=1', 'attempt.1.score=0.96902173913043477',
    'attempt.1.duration_ms=0.048917000000000002')) {
    if ($stored -cnotcontains $field) { throw "Missing saved field: $field" }
}
if (-not ($stored -match '^created_at_utc=\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}$')) {
    throw 'Missing creation timestamp'
}
Invoke-History 0 'status=replay' $saveArguments
Invoke-History 3 'status=conflict' @('save', "$run-other", $request)
Invoke-History 3 'status=conflict' @('save', $inspection, "$run-other")
Invoke-History 3 'status=conflict' ($saveArguments + 'changed-score')
Invoke-History 4 'status=not_found' @('get', "$run-other")
$replayed = @(Invoke-History 0 'status=found' $getArguments)
if (($stored -join "`n") -cne ($replayed -join "`n")) { throw 'Replay or conflict changed the original row' }

Invoke-History 1 'status=failed' @('save', "$run-rollback", "$run-rollback", 'invalid-attempt') 'Insert analysis attempt [MySQL 3819]'
Invoke-History 4 'status=not_found' @('get', "$run-rollback")
foreach ($variant in @('oversized-input', 'oversized-stage')) {
    $invalidInspection = "$run-$variant"
    $operation = if ($variant -eq 'oversized-input') { 'Insert inspection' } else { 'Insert analysis attempt' }
    Invoke-History 1 'status=failed' @('save', $invalidInspection, $invalidInspection, $variant) "$operation produced MySQL warnings"
    Invoke-History 4 'status=not_found' @('get', $invalidInspection)
}
"S05 checks passed ($run)"
