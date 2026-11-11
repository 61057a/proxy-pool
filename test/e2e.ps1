# End-to-end test with real free proxies.
# Starts proxypool on port 8090, waits for the pool to be ready, then requests
# a few URLs through it. Requires internet access.
#
#   powershell -ExecutionPolicy Bypass -File test\e2e.ps1

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "dist\proxypool.exe"
$port = 8090
$logPath = Join-Path $root "build\e2e_serve.log"

if (-not (Test-Path $exe)) {
    Write-Host "ERROR: $exe not found, run build.bat first" -ForegroundColor Red
    exit 1
}

# Kill leftovers that may hold the test port
$old = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
foreach ($c in $old) {
    Stop-Process -Id $c.OwningProcess -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 500

Write-Host "starting proxypool on port $port ..."
$p = Start-Process -FilePath $exe -ArgumentList "--port", "$port", "--max", "40" `
    -RedirectStandardOutput $logPath -NoNewWindow -PassThru

$ready = $false
for ($i = 0; $i -lt 300; $i++) {
    if ($p.HasExited) { break }
    $c = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
    if ($c -and $c.OwningProcess -eq $p.Id) { $ready = $true; break }
    Start-Sleep -Seconds 1
}

if (-not $ready) {
    Write-Host "ERROR: listener did not come up (process exited: $($p.HasExited))" -ForegroundColor Red
    if (Test-Path $logPath) { Get-Content $logPath -Encoding UTF8 | Select-Object -Last 20 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    exit 1
}
Write-Host "listener ready after $i s"

$urls = @(
    "http://www.baidu.com",
    "https://www.example.com",
    "https://www.baidu.com",
    "http://ip.3322.net"
)

$failed = 0
foreach ($u in $urls) {
    try {
        $r = Invoke-WebRequest -Uri $u -Proxy "http://127.0.0.1:$port" -TimeoutSec 25 -UseBasicParsing
        Write-Host ("PASS  {0}  status={1} len={2}" -f $u, $r.StatusCode, $r.Content.Length) -ForegroundColor Green
    } catch {
        Write-Host ("FAIL  {0}  {1}" -f $u, $_.Exception.Message) -ForegroundColor Yellow
        $failed++
    }
}

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

Write-Host ""
if ($failed -eq $urls.Count) {
    Write-Host "all requests failed - free proxies may be unusable right now" -ForegroundColor Red
    exit 1
}
Write-Host "done ($failed of $($urls.Count) requests failed)" -ForegroundColor Cyan
exit 0
