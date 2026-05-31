#requires -Version 5.1
# deploy-runtime.ps1
#
# Drop the minimum NVIDIA App runtime into the hardcoded paths the Shadowplay
# plugin loads from. This is NOT "installing NVIDIA App" — it's placing the
# narrow set of capture/encode DLLs at the locations the signed binaries
# expect (paths are hardcoded into IDA-verified call sites in
# nvspserviceplugin64.dll, ipccommon64.dll, and nvspapi64.dll).
#
# Skipped intentionally:
#   - NvApp\CEF\*                 (Chromium-rendered ALT+Z overlay — the bloat we
#                                  built this project to eliminate)
#   - NVIDIA app.exe / Backend.exe (the launcher + the GeForce-account UI)
#   - NvBackend\*                  (telemetry, profile updater, ontology)
#   - NvContainer service install  (we spawn it as a child process ourselves)
#   - Start-menu shortcuts, autostart, scheduled tasks, registry HKCR entries
#
# Reference: IDA decompile of nvspserviceplugin64!sub_18001F4C0 which calls
# SHGetKnownFolderPath(FOLDERID_ProgramFiles {905E63B6-...}) and appends
# "\NVIDIA Corporation\NVIDIA App\ShadowPlay\IpcCommon64.dll".

[CmdletBinding()]
param(
    [switch]$Force
)
$ErrorActionPreference = 'Stop'

$Src         = 'C:\Users\admin\Desktop\Standalone-Shadowplay\extracted'
$Root        = 'C:\Program Files\NVIDIA Corporation\NVIDIA App'
$ShadowPlay  = Join-Path $Root 'ShadowPlay'
$NvVAD       = Join-Path $Root 'NvVAD'
$MessageBus  = Join-Path $Root 'NvApp.MessageBus'

if ((Test-Path $Root) -and -not $Force) {
    Write-Host "[!] $Root already exists. Re-run with -Force to overwrite."
    exit 1
}

# Map of (source path under $Src, destination path under $Root, destination filename)
# Underscored '_*.dll' files in the installer are renamed during install to drop the prefix.
$plan = @(
    @{Src = 'ShadowPlay\_nvspcaps64.dll';         Dest = $ShadowPlay; Name = 'nvspcaps64.dll'},
    @{Src = 'ShadowPlay\_nvspserviceplugin64.dll'; Dest = $ShadowPlay; Name = 'nvspserviceplugin64.dll'},
    @{Src = 'ShadowPlay\nvspapi64.dll';            Dest = $ShadowPlay; Name = 'nvspapi64.dll'},
    @{Src = 'ShadowPlay\nvspcap64.dll';            Dest = $ShadowPlay; Name = 'nvspcap64.dll'},
    @{Src = 'ShadowPlay\nvspcap.dll';              Dest = $ShadowPlay; Name = 'nvspcap.dll'},
    @{Src = 'ShadowPlay\nvspscreenshot64.dll';     Dest = $ShadowPlay; Name = 'nvspscreenshot64.dll'},
    @{Src = 'ShadowPlay\nvsphelper64.exe';         Dest = $ShadowPlay; Name = 'nvsphelper64.exe'},
    @{Src = 'ShadowPlay\nvsphelperplugin64.dll';   Dest = $ShadowPlay; Name = 'nvsphelperplugin64.dll'},
    @{Src = 'ShadowPlay\ipccommon64.dll';          Dest = $ShadowPlay; Name = 'ipccommon64.dll'},
    @{Src = 'NvVAD\nvaudcap64v.dll';               Dest = $NvVAD;      Name = 'nvaudcap64v.dll'},
    @{Src = 'NvVAD\nvaudcap64a.dll';               Dest = $NvVAD;      Name = 'nvaudcap64a.dll'},
    @{Src = 'NvVAD\nvaudcap32v.dll';               Dest = $NvVAD;      Name = 'nvaudcap32v.dll'},
    @{Src = 'NvApp.MessageBus\MessageBus.dll';     Dest = $MessageBus; Name = 'MessageBus.dll'},
    @{Src = 'NvApp.MessageBus\NvMessageBus.dll';   Dest = $MessageBus; Name = 'NvMessageBus.dll'}
)

foreach ($d in @($ShadowPlay, $NvVAD, $MessageBus)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
}

foreach ($item in $plan) {
    $srcPath = Join-Path $Src $item.Src
    $dstPath = Join-Path $item.Dest $item.Name
    if (-not (Test-Path $srcPath)) {
        Write-Host "[!] missing source: $srcPath"; continue
    }
    Copy-Item -Path $srcPath -Destination $dstPath -Force
    $sz = '{0,8:N0}' -f (Get-Item $dstPath).Length
    Write-Host "  + $sz  $dstPath"
}

Write-Host ""
Write-Host "[+] Deploy complete. Standalone-Shadowplay can now spawn NvContainer in child mode."
