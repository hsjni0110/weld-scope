function Resolve-S04Python {
    param([Parameter(Mandatory)][string]$RepositoryRoot, [string]$Python)
    if ($Python) {
        if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) { throw "Python executable missing: $Python" }
        return (Resolve-Path -LiteralPath $Python).Path
    }
    foreach ($relative in @('.venv/Scripts/python.exe', '.tools/python/python.exe')) {
        $candidate = Join-Path $RepositoryRoot $relative
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    $command = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command -and $command.Source -notlike '*\WindowsApps\*') { return $command.Source }
    throw 'Python missing. Create the S02 .venv or pass -Python with an executable path.'
}
