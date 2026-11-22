[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$AppDir,
    [Parameter(Mandatory)][string]$Output
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
$manifestName = 'pooled-smooth-220.json'
$weightsName = 'model-220.txt'
$files = [ordered]@{
    'WeldScope.App.exe'           = Join-Path $app 'WeldScope.App.exe'
    $manifestName                 = Join-Path $app $manifestName
    $weightsName                  = Join-Path $app $weightsName
    'defect.png'                  = Join-Path $app 'defect.png'
    'samples/normal.png'          = Join-Path $sampleDir 'normal.png'
    'samples/corrupt.png'         = Join-Path $sampleDir 'corrupt.png'
    'db/schema.sql'               = Join-Path $repo 'db/s05/schema.sql'
    'db/s13.sql'                  = Join-Path $repo 'db/s13/schema.sql'
    'README.md'                  = Join-Path $docs 'S13-run-package.md'
    'THIRD-PARTY-NOTICES.md'      = Join-Path $docs 'S12-third-party-notices.md'
    'licenses/OpenCV.txt'        = Join-Path $opencv 'LICENSE'
    'licenses/libpng.txt'        = Join-Path $opencv 'etc/licenses/libpng-LICENSE'
    'licenses/zlib.txt'          = Join-Path $opencv 'etc/licenses/zlib-LICENSE'
    'WeldScope-Start.cmd'         = Join-Path $PSScriptRoot 'WeldScope-Start.cmd'
    'Start-WeldScope-Practice.ps1'= Join-Path $PSScriptRoot 'Start-WeldScope-Practice.ps1'
}
$expectedHashes = @{
    $manifestName = '6b85803c90e4dfa5d58ed7f181a91d5b0f3176d92522272c5cd85d0cbd9f2955'
    'defect.png' = (Get-FileHash -LiteralPath (Join-Path $sampleDir 'defect.png')).Hash
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
        if ($item.Key -eq $manifestName) {
            $model = Get-Content -LiteralPath $target -Raw -Encoding UTF8 | ConvertFrom-Json
            $expectedHashes[$weightsName] = $model.model_sha256
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
