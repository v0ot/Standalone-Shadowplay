#requires -RunAsAdministrator
#requires -Version 5.1
<#
.SYNOPSIS
    One-time setup for Standalone ShadowPlay.

.DESCRIPTION
    Deploys the minimum ShadowPlay runtime from the extracted NVIDIA App installer
    into the paths that the driver's symlinks expect. After running this, the
    NvContainerLocalSystem service will load the ShadowPlay plugins on next restart.

    Prerequisites:
    - NVIDIA display driver installed via NVCleanstall with "ShadowPlay" checked
      (provides nvcontainer.exe, NvContainerLocalSystem service, nvspcap64.dll, symlinks)
    - NVIDIA App installer extracted to ..\extracted\ (run extract-installer.ps1 first)

    What this does NOT install:
    - NVIDIA App.exe / NVIDIA Overlay.exe / CEF (the bloat)
    - NvBackend (telemetry, profile updater)
    - Any account / login / cloud features
    - Any autostart entries beyond what the driver already created
#>
[CmdletBinding()]
param(
    [string]$ExtractedPath = (Join-Path $PSScriptRoot '..\extracted'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

# Verify prerequisites
if (-not (Test-Path (Join-Path $ExtractedPath 'ShadowPlay\nvspapi64.dll'))) {
    throw "Extracted NVIDIA App payload not found at $ExtractedPath. Run extract-installer.ps1 first."
}

$svc = Get-Service 'NvContainerLocalSystem' -ErrorAction SilentlyContinue
if (-not $svc) {
    throw "NvContainerLocalSystem service not found. Install the NVIDIA driver with ShadowPlay checked in NVCleanstall."
}

Write-Host "Standalone ShadowPlay Setup" -ForegroundColor Cyan
Write-Host "==========================" -ForegroundColor Cyan
Write-Host ""

# Stop the service while we deploy
Write-Host "Stopping NvContainerLocalSystem service..."
Stop-Service 'NvContainerLocalSystem' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# ---- Deploy ShadowPlay DLLs ----
$spDst = 'C:\Program Files\NVIDIA Corporation\NVIDIA App\ShadowPlay'
$mbDst = 'C:\Program Files\NVIDIA Corporation\NVIDIA App\MessageBus'

foreach ($d in @($spDst, "$spDst\NVSPCAPS", "$spDst\Plugins\LocalSystem", $mbDst)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
}

$plan = @(
    # ShadowPlay DLLs
    @{S='ShadowPlay\nvspapi64.dll';            D=$spDst},
    @{S='ShadowPlay\ipccommon64.dll';          D=$spDst},
    @{S='ShadowPlay\nvsphelper64.exe';         D=$spDst},
    @{S='ShadowPlay\nvsphelperplugin64.dll';   D=$spDst},
    @{S='ShadowPlay\nvspscreenshot64.dll';     D=$spDst},
    @{S='ShadowPlay\nvspcap64.dll';            D=$spDst},
    @{S='ShadowPlay\nvspcap.dll';              D=$spDst},
    @{S='ShadowPlay\capcore64.dll';            D=$spDst},
    @{S='ShadowPlay\nvfcapi64.dll';            D=$spDst},
    @{S='ShadowPlay\nvfp64.dll';               D=$spDst},
    @{S='ShadowPlay\nvmf64.dll';               D=$spDst},
    # Plugins (symlink targets)
    @{S='ShadowPlay\_nvspcaps64.dll';          D="$spDst\NVSPCAPS"},
    @{S='ShadowPlay\_nvspserviceplugin64.dll'; D="$spDst\Plugins\LocalSystem"},
    # MessageBus
    @{S='NvApp.MessageBus\MessageBus.dll';     D=$mbDst},
    @{S='NvApp.MessageBus\NvMessageBus.dll';   D=$mbDst},
    @{S='NvApp.MessageBus\NvMessageBusBroadcast.dll'; D=$mbDst},
    @{S='NvApp.MessageBus\messagebus.conf';    D=$mbDst}
)

Write-Host "Deploying runtime DLLs..."
foreach ($item in $plan) {
    $src = Join-Path $ExtractedPath $item.S
    $dst = Join-Path $item.D (Split-Path $item.S -Leaf)
    if (Test-Path $src) {
        Copy-Item $src $dst -Force
        $sz = '{0,8:N0}' -f (Get-Item $dst).Length
        Write-Host "  $sz  $(Split-Path $dst -Leaf)"
    } else {
        Write-Host "  [skip] $($item.S) not found" -ForegroundColor Yellow
    }
}

# ---- Deploy nvspcap64.dll to System32 (if driver didn't) ----
if (-not (Test-Path 'C:\Windows\System32\nvspcap64.dll')) {
    Write-Host "Deploying nvspcap64.dll to System32..."
    Copy-Item (Join-Path $ExtractedPath 'ShadowPlay\nvspcap64.dll') 'C:\Windows\System32\nvspcap64.dll' -Force
}
if (-not (Test-Path 'C:\Windows\SysWOW64\nvspcap.dll')) {
    Copy-Item (Join-Path $ExtractedPath 'ShadowPlay\nvspcap.dll') 'C:\Windows\SysWOW64\nvspcap.dll' -Force
}

# ---- Deploy IpcCommon to LocalAppData (where the plugin ctor hardcodes) ----
$localSp = "$env:LOCALAPPDATA\NVIDIA Corporation\NVIDIA App\ShadowPlay"
if (-not (Test-Path $localSp)) { New-Item -ItemType Directory -Path $localSp -Force | Out-Null }
Copy-Item (Join-Path $ExtractedPath 'ShadowPlay\ipccommon64.dll') "$localSp\ipccommon64.dll" -Force
Copy-Item (Join-Path $mbDst 'messagebus.conf') "$localSp\messagebus.conf" -Force
Write-Host "  Deployed IpcCommon to LocalAppData"

# ---- Seed registry ----
Write-Host ""
Write-Host "Seeding registry..."

# ShadowPlayCfg2 (XOR-validated magic)
$spKey = 'HKLM:\SOFTWARE\NVIDIA Corporation\Global\NvApp\ShadowPlay'
if (-not (Test-Path $spKey)) { New-Item -Path $spKey -Force | Out-Null }
$target = [byte[]](0x0D,0xEF,0xC9,0x1B,0xA2,0x36,0xF3,0x4E, 0xB2,0xF9,0xF7,0x3A,0x3A,0x88,0x97,0x5E)
$cfg2 = [byte[]]::new(16)
for ($i = 0; $i -lt 16; $i++) { $cfg2[$i] = $target[$i] -bxor 0xA6 }
New-ItemProperty -Path $spKey -Name 'ShadowPlayCfg2' -PropertyType Binary -Value $cfg2 -Force | Out-Null
New-ItemProperty -Path $spKey -Name 'ShadowPlayCfg3' -PropertyType DWord -Value 3 -Force | Out-Null
Write-Host "  ShadowPlayCfg2/3 set"

# FTS feature toggle
$ftsKey = "$spKey\FTS"
if (-not (Test-Path $ftsKey)) { New-Item -Path $ftsKey -Force | Out-Null }
New-ItemProperty -Path $ftsKey -Name '{11E72C12-FE4E-4BC5-AC47-7F4BD7C948D6}' -Value 1 -PropertyType DWord -Force | Out-Null
Write-Host "  FTS toggle set"

# FullPath registry (plugin ctor reads this)
$appKey = 'HKLM:\SOFTWARE\NVIDIA Corporation\Global\NvApp'
if (-not (Test-Path $appKey)) { New-Item -Path $appKey -Force | Out-Null }
New-ItemProperty -Path $appKey -Name 'FullPath' -Value "$spDst\nvsphelper64.exe" -PropertyType String -Force | Out-Null
Write-Host "  FullPath set"

# ---- Restart service ----
Write-Host ""
Write-Host "Starting NvContainerLocalSystem service..."
Start-Service 'NvContainerLocalSystem'
Start-Sleep -Seconds 3

$svcStatus = (Get-Service 'NvContainerLocalSystem').Status
Write-Host "Service status: $svcStatus"

if ($svcStatus -eq 'Running') {
    Write-Host ""
    Write-Host "Setup complete!" -ForegroundColor Green
    Write-Host ""
    Write-Host "Run ShadowPlay.exe to start the standalone controller."
    Write-Host "  Alt+F10       Save instant replay"
    Write-Host "  Alt+F9        Toggle manual recording"
    Write-Host "  Alt+Shift+F10 Toggle instant replay on/off"
    Write-Host "  Right-click tray icon for menu"
} else {
    Write-Host "Service failed to start. Check the NvContainer log." -ForegroundColor Red
}
