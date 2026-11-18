# Starts BreakerServer from source for local development (dotnet run). Paths are relative to the repo.
#   powershell -ExecutionPolicy Bypass -File scripts\start-breaker-server.ps1 [-TailscaleIP <100.x>] [-Capture] [-StunDebug] [-CustomNews]
# Default: listens on 127.0.0.1 only (this PC). -TailscaleIP <this PC's `tailscale ip -4`>: also listens on that
# Tailscale address (never 0.0.0.0) and tells clients to use it, so another player on your tailnet can connect.
# For reviewed, reproducible builds run in a hardened container, use the breakers-sandbox tooling instead
# (run\Start-BreakerServer.ps1).
param([string]$TailscaleIP, [switch]$Capture, [switch]$StunDebug, [switch]$CustomNews)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$data = Join-Path $repo 'runtime-data'      # not committed; see runtime-data\README.md

# Listen scope. Every variable is set on each run so a previous -TailscaleIP run in this shell can't linger.
$hostIp = '127.0.0.1'
if ($TailscaleIP) {
    $ip = $null
    if (-not [Net.IPAddress]::TryParse($TailscaleIP, [ref]$ip) -or $ip.AddressFamily -ne 'InterNetwork') { throw "-TailscaleIP '$TailscaleIP' is not an IPv4 address." }
    $b = $ip.GetAddressBytes()
    if (-not ($b[0] -eq 100 -and $b[1] -ge 64 -and $b[1] -le 127)) { throw "-TailscaleIP $TailscaleIP is outside 100.64.0.0/10 (Tailscale's range). Refusing to listen on it." }
    if (-not (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object IPAddress -eq $TailscaleIP)) { throw "$TailscaleIP is not assigned to this PC. Is Tailscale connected? (tailscale ip -4)" }
    $hostIp = $TailscaleIP
}
$env:Urls = if ($TailscaleIP) { "http://127.0.0.1:5000;http://${TailscaleIP}:5000" } else { 'http://127.0.0.1:5000' }
$env:Breaker__BindAddress = $hostIp
foreach ($k in 'UdpHost', 'StunHost', 'AdvertisedHost', 'EnvHost', 'PrdHost') { Set-Item "env:Breaker__$k" $hostIp }
$env:Breaker__BackendBaseUrl = "http://${hostIp}:5000"

# Diarkis session keys: unset on purpose -> fresh random keys per session (like the official server).
Remove-Item Env:Breaker__DiarkisIvKey, Env:Breaker__DiarkisAesKey, Env:Breaker__DiarkisSidKey, Env:Breaker__DiarkisHashKey -ErrorAction SilentlyContinue

# Matchmaking needs the captured battle data, otherwise the client says "game data contains old data".
$adjust = Join-Path $data 'adjustmentdata.txt'
if (-not (Test-Path $adjust)) { throw "Missing $adjust (see runtime-data\README.md)" }
$expected = (Get-Content (Join-Path $data 'runtime-data.sha256') | Where-Object { $_ -match '  adjustmentdata\.txt$' }).Substring(0, 64)
if ((Get-FileHash $adjust -Algorithm SHA256).Hash.ToLower() -ne $expected) { throw "adjustmentdata.txt hash mismatch (expected $expected)" }
$env:Breaker__AdjustmentDataPath = $adjust

# Official save imports: drop <steamId>.json here (see docs\migrating-official-saves.md).
$env:Breaker__ImportDir = Join-Path $data 'save-imports'
New-Item -ItemType Directory -Force $env:Breaker__ImportDir | Out-Null

if ($StunDebug) { ${env:Logging__LogLevel__BreakerServer.Network.StunUdpService} = 'Debug' }
if ($CustomNews) {
    $env:Breaker__NewsImageUrl  = 'https://d1bwv3qlbl0wno.cloudfront.net/tss/025348/0/legacy-news-test-05-niko.png'
    $env:Breaker__NewsImageFile = Join-Path $data 'news\legacy-news-test-05-niko.png'
}
if ($Capture) { $env:BREAKER_CAPTURE_DIR = Join-Path $data "captures\$(Get-Date -Format 'yyyyMMdd-HHmmss')" }

Write-Host "Starting server: $env:Urls, UDP/STUN on $hostIp (fresh Diarkis keys per session) ..."
dotnet run --project (Join-Path $repo 'BreakerServer\BreakerServer\BreakerServer.csproj')
