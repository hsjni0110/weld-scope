param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('ARM64', 'x64', 'Win32')][string]$Platform = 'ARM64'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $installation) { throw 'Visual Studio MSBuild installation missing' }
$msbuild = Join-Path $installation 'MSBuild/Current/Bin/MSBuild.exe'
$output = Join-Path $repoRoot "artifacts/S03/$Platform/$Configuration/"
$intermediate = Join-Path $repoRoot "artifacts/S03/obj/$Platform/$Configuration/"
$solutionPlatform = if ($Platform -eq 'Win32') { 'x86' } else { $Platform }
New-Item -ItemType Directory -Path $output -Force | Out-Null
& $msbuild (Join-Path $repoRoot 'weld-scope.slnx') /m /t:Build "/p:Configuration=$Configuration" "/p:Platform=$solutionPlatform" "/p:OutDir=$output" "/p:IntDir=$intermediate" /verbosity:minimal /nologo /fileLogger "/flp:LogFile=${output}build.log;Verbosity=minimal"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "Application: ${output}WeldScope.App.exe"
