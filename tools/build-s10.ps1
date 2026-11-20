param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$StagedMfc,
    [string]$SelectiveFreeze,
    [string]$OutputRoot
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$components = if ($StagedMfc) { @() } else { @('-requires', 'Microsoft.VisualStudio.Component.VC.ATLMFC') }
$installation = & $vswhere -latest -products * @components -property installationPath
if (-not $installation) { throw 'Visual Studio x64 MFC component (Microsoft.VisualStudio.Component.VC.ATLMFC) is required' }
$hostTools = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
$msbuild = Join-Path $installation "MSBuild/Current/Bin/$hostTools/MSBuild.exe"
$toolset = (Get-Content (Join-Path $installation 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.v145.default.txt') -Raw).Trim()
$tools = Join-Path $installation "VC/Tools/MSVC/$toolset/bin/Host$hostTools/x64"
$stage = if ($SelectiveFreeze) { 'S11' } else { 'S10' }
$buildRoot = if ($OutputRoot) {
    $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputRoot)
} else { Join-Path $repoRoot "artifacts/$stage" }
$output = Join-Path $buildRoot "x64/$Configuration/"
$intermediate = Join-Path $buildRoot "obj/x64/$Configuration/"
$selectiveOptions = @()
if ($SelectiveFreeze) {
    $freeze = (Resolve-Path -LiteralPath $SelectiveFreeze).Path
    $freezeFolder = Split-Path $freeze -Parent
    $configPath = Join-Path $freezeFolder 'config/selective.json'
    if (!(Test-Path -LiteralPath $configPath -PathType Leaf)) {
        $configPath = Join-Path $freezeFolder 'selective.json'
    }
    $selective = (Resolve-Path -LiteralPath $configPath).Path
    $selectiveOptions = @("/p:WeldScopeS11Freeze=$freeze", "/p:WeldScopeS11Selective=$selective")
}
$mfcOptions = @()
if ($StagedMfc) {
    if ($Configuration -ne 'Release') { throw 'Staged x64 MFC libraries support Release only' }
    $mfcLib = (Resolve-Path (Join-Path $StagedMfc 'lib')).Path
    $mfcOptions = @("/p:MFC_KeyFile=$mfcLib\mfcs140u.lib", "/p:ATL_KeyFile=$mfcLib\atls.lib")
    $env:LINK = ('/LIBPATH:"{0}" {1}' -f $mfcLib, $env:LINK).TrimEnd()
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
& $msbuild (Join-Path $repoRoot 'weld-scope.slnx') /m /t:Build "/p:Configuration=$Configuration" '/p:Platform=x64' "/p:PreferredToolArchitecture=$hostTools" "/p:CLToolPath=$tools" "/p:LinkToolPath=$tools" "/p:OutDir=$output" "/p:IntDir=$intermediate" @mfcOptions @selectiveOptions /verbosity:minimal /nologo /fileLogger "/flp:LogFile=${output}build.log;Verbosity=minimal"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($StagedMfc) { Copy-Item (Join-Path $StagedMfc 'bin/*.dll') $output }
Write-Output "Application: ${output}WeldScope.App.exe"
