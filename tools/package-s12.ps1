[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$AppDir,
    [Parameter(Mandatory)][string]$Inspect,
    [Parameter(Mandatory)][string]$TwoStageSample,
    [Parameter(Mandatory)][string]$Output,
    [switch]$FolderInspection
)

$ErrorActionPreference = 'Stop'

function Assert-X64Executable([string]$Path, [string]$Name) {
    $failure = "Package input must be an x64 PE executable: $Name"
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        $stream = $reader.BaseStream
        # The DOS header stores the file offset of the PE signature.
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) {
            throw $failure
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 64 -or $peOffset -gt $stream.Length - 24) {
            throw $failure
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw $failure
        }
        $sectionCount = $reader.ReadUInt16()
        $stream.Position = $peOffset + 20
        $optionalSize = $reader.ReadUInt16()
        $characteristics = $reader.ReadUInt16()
        # PE32+ executable image, with the DLL flag clear and complete headers.
        if (($characteristics -band 0x2002) -ne 0x0002 -or
            $sectionCount -eq 0 -or $optionalSize -lt 112 -or
            ($optionalSize + 40 * $sectionCount) -gt ($stream.Length - $stream.Position) -or
            $reader.ReadUInt16() -ne 0x020B) {
            throw $failure
        }
    } finally {
        $reader.Dispose()
    }
}

$repo = Split-Path $PSScriptRoot -Parent
$app = (Resolve-Path -LiteralPath $AppDir).Path
$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
if ([IO.Path]::GetExtension($destination) -ne '.zip' -or (Test-Path -LiteralPath $destination)) {
    throw 'Output must be a new .zip file.'
}
$sampleDir = Join-Path $repo 'src/WeldScope.App/res/samples'
$opencv = Join-Path $repo '.tools/opencv/install-x64'
$docs = Join-Path $repo 'docs/steps'
$files = [ordered]@{
    'WeldScope.App.exe'        = Join-Path $app 'WeldScope.App.exe'
    'weldscope_inspect.exe'    = $Inspect
    'baseline.json'            = Join-Path $app 'baseline.json'
    'freeze.json'              = Join-Path $app 'freeze.json'
    'selective.json'           = Join-Path $app 'selective.json'
    'defect.png'               = Join-Path $app 'defect.png'
    'samples/normal.png'       = Join-Path $sampleDir 'normal.png'
    'samples/image_10513.png'  = $TwoStageSample
    'samples/corrupt.png'      = Join-Path $sampleDir 'corrupt.png'
    'db/schema.sql'            = Join-Path $repo 'db/s05/schema.sql'
    'README.md'                = Join-Path $docs 'S12-run-package.md'
    'THIRD-PARTY-NOTICES.md'   = Join-Path $docs 'S12-third-party-notices.md'
    'licenses/OpenCV.txt'     = Join-Path $opencv 'LICENSE'
    'licenses/libpng.txt'     = Join-Path $opencv 'etc/licenses/libpng-LICENSE'
    'licenses/zlib.txt'       = Join-Path $opencv 'etc/licenses/zlib-LICENSE'
}
if ($FolderInspection) {
    $files['README.md'] = Join-Path $docs 'S13-run-package.md'
    $files['db/s13.sql'] = Join-Path $repo 'db/s13/schema.sql'
    $files['WeldScope-Start.cmd'] = Join-Path $PSScriptRoot 'WeldScope-Start.cmd'
    $files['Start-WeldScope-Practice.ps1'] = Join-Path $PSScriptRoot 'Start-WeldScope-Practice.ps1'
}

$freezePath = Join-Path $docs 'S11-evidence/freeze.json'
$freeze = Get-Content -LiteralPath $freezePath -Raw -Encoding UTF8 | ConvertFrom-Json
$demo = Get-Content -LiteralPath (Join-Path $docs 'S11-evidence/demo/comparison.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$expectedHashes = @{
    'freeze.json'             = (Get-FileHash -LiteralPath $freezePath).Hash
    'baseline.json'           = $freeze.config_sha256.baseline
    'selective.json'          = $freeze.config_sha256.selective
    'defect.png'              = (Get-FileHash -LiteralPath (Join-Path $sampleDir 'defect.png')).Hash
    'samples/image_10513.png' = $demo.modes.selective.image_sha256
}

$parent = [IO.Path]::GetDirectoryName($destination)
[IO.Directory]::CreateDirectory($parent) | Out-Null
$temporary = Join-Path $parent ('.s12-' + [guid]::NewGuid().ToString('N'))
try {
    $payload = Join-Path $temporary 'payload'
    $hashes = foreach ($item in $files.GetEnumerator()) {
        if (!(Test-Path -LiteralPath $item.Value -PathType Leaf)) {
            throw "Missing package input: $($item.Key)"
        }
        $target = Join-Path $payload $item.Key
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        Copy-Item -LiteralPath $item.Value -Destination $target
        if ($item.Key.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) {
            Assert-X64Executable $target $item.Key
        }
        $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if ($expectedHashes.ContainsKey($item.Key) -and $hash -ne $expectedHashes[$item.Key]) {
            throw "Package input SHA-256 mismatch: $($item.Key)"
        }
        "$($hash.ToLowerInvariant())  $($item.Key)"
    }
    $hashes | Set-Content -LiteralPath (Join-Path $payload 'SHA256SUMS.txt') -Encoding ASCII
    $archive = Join-Path $temporary 'package.zip'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($payload, $archive)
    [IO.File]::Move($archive, $destination)
    Write-Output $destination
} finally {
    if (Test-Path -LiteralPath $temporary) {
        $resolvedTemporary = (Resolve-Path -LiteralPath $temporary).Path
        $resolvedParent = (Resolve-Path -LiteralPath $parent).Path
        if ([IO.Path]::GetDirectoryName($resolvedTemporary) -eq $resolvedParent) {
            Remove-Item -LiteralPath $resolvedTemporary -Recurse -Force
        }
    }
}
