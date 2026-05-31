#requires -RunAsAdministrator
#requires -Version 5.1
<#
.SYNOPSIS
    Complete standalone ShadowPlay setup — no ShadowPlay checkbox needed.

.DESCRIPTION
    Deploys all ShadowPlay components from the extracted NVIDIA App installer.
    Only requires the base NVIDIA display driver (install via NVCleanstall
    with EVERYTHING unchecked except the display driver itself).

    This script installs:
    - NvContainer.exe and registers it as a Windows service
    - All ShadowPlay capture DLLs to their canonical paths
    - MessageBus broadcaster + config
    - nvspcap64.dll to System32 (D3D game-injection proxy)
    - Plugin directory structure with symlinks
    - Watchdog registry entries for auto-launching user containers
    - ShadowPlay registry configuration (Cfg2/Cfg3, FTS toggles)

    This script does NOT install:
    - NVIDIA App.exe / NVIDIA Overlay.exe / CEF
    - NvBackend (telemetry, profile updater)
    - Any account, login, or cloud features
    - Any tray icons or overlays (that's what ShadowPlay.exe replaces)

.PARAMETER ExtractedPath
    Path to the extracted NVIDIA App installer payload. Default: ..\extracted

.PARAMETER Uninstall
    Remove everything this script installed.

.EXAMPLE
    .\setup.ps1
    .\setup.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$ExtractedPath = (Join-Path $PSScriptRoot '..\extracted'),
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$NvContainerDir = 'C:\Program Files\NVIDIA Corporation\NvContainer'
$NvAppDir       = 'C:\Program Files\NVIDIA Corporation\NVIDIA App'
$SpDir          = "$NvAppDir\ShadowPlay"
$MbDir          = "$NvAppDir\MessageBus"
$PluginBase     = "$NvContainerDir\plugins"
$SvcName        = 'NvContainerLocalSystem'

# ============================================================
# UNINSTALL
# ============================================================
if ($Uninstall) {
    Write-Host "Uninstalling Standalone ShadowPlay..." -ForegroundColor Yellow

    $svc = Get-Service $SvcName -ErrorAction SilentlyContinue
    if ($svc) {
        Stop-Service $SvcName -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
        sc.exe delete $SvcName | Out-Null
        Write-Host "  Service removed"
    }

    Get-Process -Name 'NvContainer','nvsphelper64' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($NvContainerDir) } |
        Stop-Process -Force -ErrorAction SilentlyContinue

    foreach ($p in @(
        'C:\Windows\System32\nvspcap64.dll',
        'C:\Windows\SysWOW64\nvspcap.dll'
    )) { Remove-Item $p -Force -ErrorAction SilentlyContinue }

    Remove-Item $NvContainerDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $NvAppDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item "$env:LOCALAPPDATA\NVIDIA Corporation\NVIDIA App" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKLM:\SOFTWARE\NVIDIA Corporation\Global\NvApp' -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKLM:\SOFTWARE\NVIDIA Corporation\NvContainer' -Recurse -Force -ErrorAction SilentlyContinue

    Write-Host "Uninstall complete." -ForegroundColor Green
    return
}

# ============================================================
# INSTALL
# ============================================================
if (-not (Test-Path (Join-Path $ExtractedPath 'ShadowPlay\nvspapi64.dll'))) {
    throw "Extracted NVIDIA App payload not found at $ExtractedPath.`nRun: .\extract-installer.ps1 -InstallerPath <NVIDIA_app_vXX.exe>"
}

Write-Host ""
Write-Host "  Standalone ShadowPlay Setup" -ForegroundColor Cyan
Write-Host "  ===========================" -ForegroundColor Cyan
Write-Host ""

# Stop existing service if present
$svc = Get-Service $SvcName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') {
    Write-Host "Stopping existing $SvcName..."
    Stop-Service $SvcName -Force
    Start-Sleep -Seconds 2
}

# ---- 1. Deploy NvContainer.exe ----
Write-Host "[1/7] Deploying NvContainer.exe..."
New-Item -ItemType Directory -Path $NvContainerDir -Force | Out-Null
Copy-Item (Join-Path $ExtractedPath 'NvContainer\x86_64\NvContainer.exe') "$NvContainerDir\NvContainer.exe" -Force
Copy-Item (Join-Path $ExtractedPath 'NvContainer\x86_64\NvPluginWatchdog.dll') "$NvContainerDir\NvPluginWatchdog.dll" -Force

# ---- 2. Deploy ShadowPlay DLLs ----
Write-Host "[2/7] Deploying ShadowPlay runtime..."
foreach ($d in @($SpDir, "$SpDir\NVSPCAPS", "$SpDir\Plugins\LocalSystem", $MbDir)) {
    New-Item -ItemType Directory -Path $d -Force | Out-Null
}

$copyMap = @{
    "$SpDir"                      = @('nvspapi64.dll','ipccommon64.dll','nvsphelper64.exe',
                                      'nvsphelperplugin64.dll','nvspscreenshot64.dll',
                                      'nvspcap64.dll','nvspcap.dll','capcore64.dll',
                                      'nvfcapi64.dll','nvfp64.dll','nvmf64.dll')
    "$SpDir\NVSPCAPS"             = @('_nvspcaps64.dll')
    "$SpDir\Plugins\LocalSystem"  = @('_nvspserviceplugin64.dll')
}

foreach ($dest in $copyMap.Keys) {
    foreach ($f in $copyMap[$dest]) {
        $src = Join-Path $ExtractedPath "ShadowPlay\$f"
        if (Test-Path $src) { Copy-Item $src (Join-Path $dest $f) -Force }
    }
}

foreach ($f in @('MessageBus.dll','NvMessageBus.dll','NvMessageBusBroadcast.dll','messagebus.conf')) {
    Copy-Item (Join-Path $ExtractedPath "NvApp.MessageBus\$f") (Join-Path $MbDir $f) -Force
}

# ---- 3. Deploy to System32 ----
Write-Host "[3/7] Deploying D3D proxies to System32..."
Copy-Item (Join-Path $ExtractedPath 'ShadowPlay\nvspcap64.dll') 'C:\Windows\System32\nvspcap64.dll' -Force
Copy-Item (Join-Path $ExtractedPath 'ShadowPlay\nvspcap.dll') 'C:\Windows\SysWOW64\nvspcap.dll' -Force

# ---- 4. Create plugin directory structure with symlinks ----
Write-Host "[4/7] Creating plugin directories and symlinks..."
foreach ($d in @('LocalSystem','SPUser','User','Session','AIUser')) {
    New-Item -ItemType Directory -Path "$PluginBase\$d" -Force | Out-Null
}

# Symlinks (same as what the driver installer creates)
$symlinks = @(
    @{Link="$PluginBase\LocalSystem\ShadowPlay";           Target="$SpDir\Plugins\LocalSystem"},
    @{Link="$PluginBase\SPUser\nvspcaps";                  Target="$SpDir\NVSPCAPS"},
    @{Link="$PluginBase\LocalSystem\NvMessageBusBroadcast.dll"; Target="$MbDir\NvMessageBusBroadcast.dll"},
    @{Link="$PluginBase\LocalSystem\messagebus.conf";      Target="$MbDir\messagebus.conf"},
    @{Link="$PluginBase\LocalSystem\Watchdog";             Target="$NvContainerDir\Watchdog"}
)

# Watchdog dir
New-Item -ItemType Directory -Path "$NvContainerDir\Watchdog" -Force | Out-Null
Copy-Item "$NvContainerDir\NvPluginWatchdog.dll" "$NvContainerDir\Watchdog\NvPluginWatchdog.dll" -Force

foreach ($sl in $symlinks) {
    # Remove existing (might be a stale symlink or dir)
    if (Test-Path $sl.Link) {
        cmd.exe /c "rmdir `"$($sl.Link)`"" 2>$null
        Remove-Item $sl.Link -Force -Recurse -ErrorAction SilentlyContinue
    }
    cmd.exe /c "mklink /D `"$($sl.Link)`" `"$($sl.Target)`"" 2>$null | Out-Null
    if (-not (Test-Path $sl.Link)) {
        # File symlink for .dll/.conf
        cmd.exe /c "mklink `"$($sl.Link)`" `"$($sl.Target)`"" 2>$null | Out-Null
    }
}

# ---- 5. Deploy IpcCommon to LocalAppData ----
Write-Host "[5/7] Deploying IPC to LocalAppData..."
$localSp = "$env:LOCALAPPDATA\NVIDIA Corporation\NVIDIA App\ShadowPlay"
New-Item -ItemType Directory -Path $localSp -Force | Out-Null
Copy-Item "$SpDir\ipccommon64.dll" "$localSp\ipccommon64.dll" -Force
Copy-Item "$MbDir\messagebus.conf" "$localSp\messagebus.conf" -Force

# ---- 6. Registry ----
Write-Host "[6/7] Configuring registry..."
$spKey = 'HKLM:\SOFTWARE\NVIDIA Corporation\Global\NvApp\ShadowPlay'
$ftsKey = "$spKey\FTS"
$appKey = 'HKLM:\SOFTWARE\NVIDIA Corporation\Global\NvApp'
$wdKey  = 'HKLM:\SOFTWARE\NVIDIA Corporation\NvContainer\Watchdog'

foreach ($k in @($spKey, $ftsKey, $appKey, $wdKey)) {
    if (-not (Test-Path $k)) { New-Item -Path $k -Force | Out-Null }
}

# ShadowPlayCfg2 (XOR-validated magic)
$target = [byte[]](0x0D,0xEF,0xC9,0x1B,0xA2,0x36,0xF3,0x4E, 0xB2,0xF9,0xF7,0x3A,0x3A,0x88,0x97,0x5E)
$cfg2 = [byte[]]::new(16)
for ($i = 0; $i -lt 16; $i++) { $cfg2[$i] = $target[$i] -bxor 0xA6 }
New-ItemProperty -Path $spKey -Name 'ShadowPlayCfg2' -PropertyType Binary -Value $cfg2 -Force | Out-Null
New-ItemProperty -Path $spKey -Name 'ShadowPlayCfg3' -PropertyType DWord -Value 3 -Force | Out-Null
New-ItemProperty -Path $ftsKey -Name '{11E72C12-FE4E-4BC5-AC47-7F4BD7C948D6}' -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $appKey -Name 'FullPath' -Value "$SpDir\nvsphelper64.exe" -PropertyType String -Force | Out-Null

# Watchdog entries (auto-launch SPUser container at login)
$spuserWd = "$wdKey\SPUserX64"
if (-not (Test-Path $spuserWd)) { New-Item -Path $spuserWd -Force | Out-Null }
New-ItemProperty -Path $spuserWd -Name '(Default)' -Value 'Temp - SP User Plugins (x64)' -PropertyType String -Force | Out-Null
New-ItemProperty -Path $spuserWd -Name 'Container' -Value "$NvContainerDir\NvContainer.exe" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $spuserWd -Name 'Folder' -Value "$PluginBase\SPUser" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $spuserWd -Name 'Parameters' -Value "-f `"C:\ProgramData\NVIDIA Corporation\NVIDIA App\NvContainer\NvContainerUser%dSPUser.log`" -d `"$PluginBase\SPUser`" -r -l 3 -p 30000" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $spuserWd -Name 'Policy' -Value '10/300/5' -PropertyType String -Force | Out-Null

# Log directory
New-Item -ItemType Directory -Path 'C:\ProgramData\NVIDIA Corporation\NVIDIA App\NvContainer' -Force | Out-Null
New-Item -ItemType Directory -Path 'C:\ProgramData\NVIDIA Corporation\NVIDIA App\MessageBus' -Force | Out-Null

# ---- 7. Register + start service ----
Write-Host "[7/7] Registering NvContainerLocalSystem service..."
$existingSvc = Get-Service $SvcName -ErrorAction SilentlyContinue
if ($existingSvc) {
    sc.exe delete $SvcName | Out-Null
    Start-Sleep -Seconds 1
}

$svcBin = "`"$NvContainerDir\NvContainer.exe`" -s $SvcName -a -f `"C:\ProgramData\NVIDIA Corporation\NVIDIA App\NvContainer\NvContainerLocalSystem.log`" -l 3 -d `"$PluginBase\LocalSystem`" -r -p 30000 -ert"
New-Service -Name $SvcName -BinaryPathName $svcBin -DisplayName 'NVIDIA LocalSystem Container' -StartupType Automatic -Description 'Standalone ShadowPlay - NvContainer LocalSystem service' | Out-Null
Start-Service $SvcName

Start-Sleep -Seconds 3
$status = (Get-Service $SvcName).Status

Write-Host ""
if ($status -eq 'Running') {
    Write-Host "  Setup complete!" -ForegroundColor Green
    Write-Host ""
    Write-Host "  Service: $SvcName ($status)"
    Write-Host ""
    Write-Host "  Next: Run bin\ShadowPlay.exe"
    Write-Host "    Alt+F10        Save instant replay"
    Write-Host "    Alt+F9         Toggle manual recording"
    Write-Host "    Alt+Shift+F10  Toggle instant replay on/off"
    Write-Host "    Right-click tray icon for menu + settings"
    Write-Host ""
    Write-Host "  To uninstall: .\setup.ps1 -Uninstall"
} else {
    Write-Host "  Service failed to start (status: $status)" -ForegroundColor Red
    Write-Host "  Check: C:\ProgramData\NVIDIA Corporation\NVIDIA App\NvContainer\NvContainerLocalSystem.log"
}
