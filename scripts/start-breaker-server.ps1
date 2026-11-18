# Starts BreakerServer from source for local development (dotnet run). Paths are relative to the repo.
#   powershell -ExecutionPolicy Bypass -File scripts\start-breaker-server.ps1 [-Capture] [-StunDebug] [-CustomNews]
# For reviewed, reproducible builds run in a hardened container, use the breakers-sandbox tooling instead
# (run\Start-BreakerServer.ps1).
param([switch]$Capture, [switch]$StunDebug, [switch]$CustomNews)
$repo = Split-Path $PSScriptRoot -Parent
$data = Join-Path $repo 'runtime-data'      # not committed; see runtime-data\README.md

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

Write-Host 'Starting server on http://127.0.0.1:5000 (fresh Diarkis keys per session) ...'
dotnet run --project (Join-Path $repo 'BreakerServer\BreakerServer\BreakerServer.csproj')
