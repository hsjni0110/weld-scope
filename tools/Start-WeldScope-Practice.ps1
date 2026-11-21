$ErrorActionPreference = 'Stop'
$appPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'WeldScope.App.exe')).Path
$mysqlHome = $env:WELDSCOPE_MYSQL_HOME
if (!$mysqlHome) {
    for ($folder = Get-Item -LiteralPath $PSScriptRoot; $folder; $folder = $folder.Parent) {
        $localTools = Join-Path $folder.FullName '.tools/mysql-8.4.13-winx64'
        if (Test-Path -LiteralPath (Join-Path $localTools 'bin/mysqld.exe') -PathType Leaf) {
            $mysqlHome = $localTools
            break
        }
    }
}
if (!$mysqlHome) { throw 'Set WELDSCOPE_MYSQL_HOME to the root of the MySQL ZIP installation.' }
$mysql = (Resolve-Path -LiteralPath $mysqlHome).Path
$open = Get-Process -Name WeldScope.App -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $appPath -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($open) {
    (New-Object -ComObject WScript.Shell).AppActivate($open.Id) | Out-Null
    return
}

$mutex = [Threading.Mutex]::new($false, 'Local\WeldScopePracticeLauncher')
$ownsMutex = $false
$server = $null
try {
    try { $ownsMutex = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (!$ownsMutex) { return }

    Write-Host 'Starting WeldScope practice...'
    $client = Join-Path $mysql 'bin/mysql.exe'
    $serverExe = Join-Path $mysql 'bin/mysqld.exe'
    $mysqladmin = Join-Path $mysql 'bin/mysqladmin.exe'
    $port = 33371
    $admin = @('--no-defaults', '--protocol=TCP', '-h', '127.0.0.1', '-P', "$port", '-u', 'root')
    $probe = [Net.Sockets.TcpClient]::new()
    try {
        try { $probe.Connect('127.0.0.1', $port) } catch [Net.Sockets.SocketException] {}
        if ($probe.Connected) { throw "Practice MySQL port $port is already in use." }
    } finally { $probe.Dispose() }

    $data = Join-Path $PSScriptRoot 'practice-launcher-data'
    $argsForServer = @('--no-defaults', ('"--basedir=' + $mysql + '"'), ('"--datadir=' + $data + '"'),
                      ('"--log-error=' + (Join-Path $PSScriptRoot 'practice-launcher-mysql.log') + '"'))
    if (!(Test-Path -LiteralPath (Join-Path $data 'mysql') -PathType Container)) {
        $init = Start-Process -FilePath $serverExe -ArgumentList ($argsForServer + '--initialize-insecure') -WindowStyle Hidden -Wait -PassThru
        if ($init.ExitCode -ne 0) { throw 'Practice MySQL initialization failed.' }
    }
    $server = Start-Process -FilePath $serverExe -ArgumentList ($argsForServer + @('--bind-address=127.0.0.1', "--port=$port", '--mysqlx=OFF', '--tls-version=')) -WindowStyle Hidden -PassThru
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        & $mysqladmin @admin --silent --connect-timeout=1 ping | Out-Null
        if ($LASTEXITCODE -eq 0) { break }
        if ($server.HasExited) { throw 'Practice MySQL did not start.' }
        Start-Sleep -Milliseconds 250
    }
    if ($LASTEXITCODE -ne 0 -or $server.HasExited) { throw 'Practice MySQL did not start.' }
    $actualData = & $client @admin --raw -Nse 'SELECT @@datadir;'
    if ($LASTEXITCODE -ne 0 -or $actualData.TrimEnd([char[]]'\/') -ne $data) {
        throw 'The running MySQL server is not the practice instance.'
    }
    $ownsServer = $true
    $env:WELDSCOPE_DEVICE_PORT = '0'
    $env:WELDSCOPE_MYSQL_HOST = '127.0.0.1'
    $env:WELDSCOPE_MYSQL_PORT = "$port"
    $env:WELDSCOPE_MYSQL_DATABASE = 'weldscopepractice'
    $env:WELDSCOPE_MYSQL_USER = 'weldscope_practice'
    $env:WELDSCOPE_MYSQL_PASSWORD = [guid]::NewGuid().ToString('N')
    Remove-Item Env:WELDSCOPE_MYSQL_CA_FILE -ErrorAction SilentlyContinue
    "CREATE DATABASE IF NOT EXISTS weldscopepractice; CREATE USER IF NOT EXISTS 'weldscope_practice'@'localhost' IDENTIFIED BY '$env:WELDSCOPE_MYSQL_PASSWORD'; ALTER USER 'weldscope_practice'@'localhost' IDENTIFIED BY '$env:WELDSCOPE_MYSQL_PASSWORD'; GRANT SELECT, INSERT ON weldscopepractice.* TO 'weldscope_practice'@'localhost';" | & $client @admin
    if ($LASTEXITCODE -ne 0) { throw 'Practice database setup failed.' }
    foreach ($schema in 'db/schema.sql', 'db/s13.sql') {
        Get-Content -LiteralPath (Join-Path $PSScriptRoot $schema) -Raw | & $client @admin weldscopepractice
        if ($LASTEXITCODE -ne 0) { throw "Practice schema setup failed: $schema" }
    }
    "GRANT UPDATE (state) ON weldscopepractice.inspection_batches TO 'weldscope_practice'@'localhost';" | & $client @admin
    if ($LASTEXITCODE -ne 0) { throw 'Practice batch permission setup failed.' }
    foreach ($dll in 'lib/libmysql.dll', 'bin/libssl-3-x64.dll', 'bin/libcrypto-3-x64.dll') {
        $source = Join-Path $mysql $dll
        $destination = Join-Path $PSScriptRoot ([IO.Path]::GetFileName($source))
        if (!(Test-Path -LiteralPath $destination -PathType Leaf)) {
            Copy-Item -LiteralPath $source -Destination $destination
        }
    }

    $env:MYSQL_PWD = $env:WELDSCOPE_MYSQL_PASSWORD
    try {
        & $client --no-defaults --protocol=TCP --get-server-public-key -h 127.0.0.1 -P $port -u weldscope_practice weldscopepractice -Nse 'SELECT 1;' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Practice app account login failed.' }
    } finally { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue }

    Start-Process -FilePath $appPath -WorkingDirectory (Split-Path $appPath -Parent) -Wait
} finally {
    if ($server -and !$server.HasExited) {
        if ($ownsServer) {
            & $mysqladmin @admin shutdown | Out-Null
        } else {
            Stop-Process -Id $server.Id
        }
    }
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
