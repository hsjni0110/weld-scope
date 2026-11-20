param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release', [ValidateSet('ARM64', 'x64')][string]$Platform = 'ARM64')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$dependencyRoot = Join-Path $repoRoot '.tools/opencv'
$archive = Join-Path $dependencyRoot 'opencv-4.13.0.zip'
$source = Join-Path $dependencyRoot 'opencv-4.13.0'
$architecture = $Platform.ToLowerInvariant()
$build = Join-Path $dependencyRoot "build-$architecture"
$install = Join-Path $dependencyRoot $(if ($Configuration -eq 'Debug') { "install-$architecture-debug" } else { "install-$architecture" })
$cmake = 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
New-Item -ItemType Directory -Force -Path $dependencyRoot | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    & curl.exe --fail --location --retry 3 --connect-timeout 30 --output "${archive}.part" 'https://codeload.github.com/opencv/opencv/zip/refs/tags/4.13.0'
    if ($LASTEXITCODE -ne 0) { throw 'OpenCV source download failed' }
    Move-Item -LiteralPath "${archive}.part" -Destination $archive
}
$digest = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($digest -ne '431cb489d275da26526581c6735d7d7c0b00f190fe940b3217d386b77aef52c7') { throw 'OpenCV source SHA-256 mismatch' }
@{version='4.13.0'; url='https://codeload.github.com/opencv/opencv/zip/refs/tags/4.13.0'; sha256=$digest} | ConvertTo-Json | Set-Content (Join-Path $dependencyRoot 'source.json') -Encoding UTF8
if (-not (Test-Path -LiteralPath $source)) { Expand-Archive -LiteralPath $archive -DestinationPath $dependencyRoot }
# CMake otherwise reports the ARM64 host CPU for this Visual Studio cross-target.
$targetOptions = if ($Platform -eq 'x64') { @('-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_SYSTEM_PROCESSOR=AMD64') }
& $cmake @targetOptions -S $source -B $build -G 'Visual Studio 18 2026' -A $Platform "-DCMAKE_INSTALL_PREFIX=$install" '-DBUILD_LIST=core,imgproc,imgcodecs' -DBUILD_SHARED_LIBS=OFF -DBUILD_WITH_STATIC_CRT=OFF -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF '-DCMAKE_POLICY_VERSION_MINIMUM=3.10' -DCMAKE_POLICY_DEFAULT_CMP0177=NEW -DWITH_ADE=OFF -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_OPENCL=OFF -DWITH_JPEG=OFF -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_OPENEXR=OFF -DWITH_OPENJPEG=OFF -DWITH_JASPER=OFF -DWITH_AVIF=OFF -DWITH_JPEGXL=OFF -DWITH_DIRECTX=OFF -DWITH_PROTOBUF=OFF -DWITH_PNG=ON -DBUILD_PNG=ON -DBUILD_ZLIB=ON
if ($LASTEXITCODE -ne 0) { throw 'OpenCV configuration failed' }
& $cmake --build $build --config $Configuration --target INSTALL --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'OpenCV build failed' }
Write-Output "OpenCV installed: $install"
