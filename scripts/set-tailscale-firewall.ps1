<#
.SYNOPSIS
  Windows Firewall rules for a Breakers test session over Tailscale. Run as Administrator.

.DESCRIPTION
  The DLL and server already bind only to the address you configure. These rules are a second,
  independent boundary that doesn't depend on the mod code behaving.

  -Server   (PC running BreakerServer)  inbound TCP 5000, UDP 7099/7100/3478/3479 allowed ONLY from
            Tailscale addresses (100.64.0.0/10) on the Tailscale interface.
  -GameExe  (every player's PC)  inbound UDP for the game allowed only from Tailscale addresses, and inbound
            from anywhere else blocked. With -BlockGameOutbound, also block the game's outbound IPv4 traffic to
            everything except Tailscale addresses (loopback is not filtered by Windows Firewall, so a local
            server keeps working). This is the external check that F-03 asks for: the DLL's own hooks
            don't cover every API.
  -Remove   deletes every rule this script created (group "Breakers Tailscale test").

  If Windows shows "allow this app on public/private networks?" when the game starts, click Cancel; the
  rules here are the ones that should decide.

.EXAMPLE
  .\scripts\set-tailscale-firewall.ps1 -Server
  .\scripts\set-tailscale-firewall.ps1 -GameExe "<game folder>\bin\DRAGON BALL THE BREAKERS.exe" -BlockGameOutbound
  (use the exe that sits next to the modded steam_api64.dll)
  .\scripts\set-tailscale-firewall.ps1 -Remove
#>
[CmdletBinding(DefaultParameterSetName = 'Add')]
param(
    [Parameter(ParameterSetName = 'Add')][switch]$Server,
    [Parameter(ParameterSetName = 'Add')][string]$GameExe,
    [Parameter(ParameterSetName = 'Add')][switch]$BlockGameOutbound,
    [Parameter(ParameterSetName = 'Add')][string]$InterfaceAlias = 'Tailscale',
    [Parameter(ParameterSetName = 'Remove')][switch]$Remove
)
$ErrorActionPreference = 'Stop'
$group = 'Breakers Tailscale test'
$tailnet = '100.64.0.0/10'
# Every IPv4 address except 100.64.0.0/10.
$notTailnet = @('0.0.0.0-100.63.255.255', '100.128.0.0-255.255.255.255')

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'Run this from an elevated PowerShell (Run as Administrator).' }

if ($Remove) {
    $rules = @(Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue)
    $rules | ForEach-Object { Write-Host "removing: $($_.DisplayName)" }
    $rules | Remove-NetFirewallRule
    Write-Host "Removed $($rules.Count) rule(s)." -ForegroundColor Green
    return
}
if (-not $Server -and -not $GameExe) { throw 'Nothing to do: pass -Server and/or -GameExe <path to the game exe> (or -Remove).' }
if (-not (Get-NetAdapter -Name $InterfaceAlias -ErrorAction SilentlyContinue)) {
    throw "No network adapter named '$InterfaceAlias'. Is Tailscale installed? (Get-NetAdapter lists the names; pass -InterfaceAlias.)"
}

function Add-Rule([hashtable]$p) {
    $existing = Get-NetFirewallRule -DisplayName $p.DisplayName -ErrorAction SilentlyContinue
    if ($existing) { $existing | Remove-NetFirewallRule }
    New-NetFirewallRule -Group $group -Profile Any @p | Out-Null
    Write-Host ("{0,-6} {1,-8} {2}" -f $p.Action, $p.Direction, $p.DisplayName)
}

if ($Server) {
    Add-Rule @{ DisplayName = 'Breakers server HTTP 5000 (tailnet only)'; Direction = 'Inbound'; Action = 'Allow'
                Protocol = 'TCP'; LocalPort = '5000'; RemoteAddress = $tailnet; InterfaceAlias = $InterfaceAlias }
    Add-Rule @{ DisplayName = 'Breakers server UDP 7099-7100,3478-3479 (tailnet only)'; Direction = 'Inbound'; Action = 'Allow'
                Protocol = 'UDP'; LocalPort = @('7099', '7100', '3478', '3479'); RemoteAddress = $tailnet; InterfaceAlias = $InterfaceAlias }
}

if ($GameExe) {
    if (-not (Test-Path -LiteralPath $GameExe -PathType Leaf)) { throw "Game exe not found: $GameExe" }
    $exe = (Resolve-Path -LiteralPath $GameExe).Path
    Add-Rule @{ DisplayName = 'Breakers game UDP in (tailnet only)'; Direction = 'Inbound'; Action = 'Allow'
                Program = $exe; Protocol = 'UDP'; RemoteAddress = $tailnet; InterfaceAlias = $InterfaceAlias }
    Add-Rule @{ DisplayName = 'Breakers game: block inbound from non-tailnet'; Direction = 'Inbound'; Action = 'Block'
                Program = $exe; RemoteAddress = $notTailnet }
    if ($BlockGameOutbound) {
        Add-Rule @{ DisplayName = 'Breakers game: block outbound to non-tailnet IPv4'; Direction = 'Outbound'; Action = 'Block'
                    Program = $exe; RemoteAddress = $notTailnet }
    }
}
Write-Host "Done. Check with: Get-NetFirewallRule -Group '$group' | Format-Table DisplayName,Direction,Action,Enabled" -ForegroundColor Green
