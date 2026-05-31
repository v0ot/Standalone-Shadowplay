#requires -Version 5.1
# run-as-system.ps1
#
# Launch NvContainer in child mode under the SYSTEM account using a one-shot
# scheduled task. We do NOT register a permanent service — the task lives
# only long enough to fire the process, then we delete it.
#
# This tests the LocalSystem hypothesis: the ShadowPlay plugin uses
# WTSQueryUserToken / CreateProcessInSession / nvCreateProcessAsUser which
# normally require the LocalSystem context. Without it, capture-session
# mutating calls hang forever.
#
# Requires admin rights to create a SYSTEM task.

[CmdletBinding()]
param(
    [string]$NvContainerDir = 'C:\Users\admin\Desktop\Standalone-Shadowplay\bin\nvcontainer',
    [string]$TaskName = 'StandaloneShadowplay_NvContainer',
    [switch]$Stop
)

$ErrorActionPreference = 'Stop'

if ($Stop) {
    Write-Host "Stopping NvContainer processes..."
    Get-Process -Name 'NvContainer' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Host "Removing scheduled task '$TaskName' if present..."
    try { cmd.exe /c "schtasks /delete /tn `"$TaskName`" /f >NUL 2>&1" } catch {}
    Write-Host "[+] cleanup complete"
    return
}

$exe  = Join-Path $NvContainerDir 'NvContainer.exe'
$args = '-c -d plugins -f nvc.log -l 7 -r'

if (-not (Test-Path $exe)) { throw "NvContainer.exe not found at $exe" }

# Wipe any prior task with this name and any running NvContainer.
try { cmd.exe /c "schtasks /delete /tn `"$TaskName`" /f >NUL 2>&1" } catch {}
Get-Process -Name 'NvContainer' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Write-Host "Creating one-shot SYSTEM scheduled task: $TaskName"
$action  = New-ScheduledTaskAction -Execute $exe -Argument $args -WorkingDirectory $NvContainerDir
$trigger = New-ScheduledTaskTrigger -Once -At ((Get-Date).AddYears(1))   # never auto-fires
$princ   = New-ScheduledTaskPrincipal -UserId 'NT AUTHORITY\SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$set     = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -Hidden:$true
$task    = New-ScheduledTask -Action $action -Trigger $trigger -Principal $princ -Settings $set
Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null

Write-Host "Triggering task manually..."
Start-ScheduledTask -TaskName $TaskName

# Wait until NvContainer.exe shows up (max 10s).
$nvProc = $null
for ($i = 0; $i -lt 50; $i++) {
    $nvProc = Get-Process -Name 'NvContainer' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($nvProc) { break }
    Start-Sleep -Milliseconds 200
}

if ($nvProc) {
    # Confirm it's running as SYSTEM
    $owner = (Get-CimInstance -Query "SELECT * FROM Win32_Process WHERE ProcessId=$($nvProc.Id)" |
              Invoke-CimMethod -MethodName GetOwner -ErrorAction SilentlyContinue).User
    Write-Host "[+] NvContainer PID=$($nvProc.Id) Owner=$owner"
} else {
    Write-Host "[!] NvContainer process did not appear after 10s"
    schtasks /query /tn $TaskName /v /fo LIST 2>$null | Select-String 'Last Result|Status'
    throw "NvContainer failed to launch as SYSTEM"
}

Write-Host "[+] NvContainer running as SYSTEM. Run probe.exe next."
Write-Host "    Cleanup: .\tools\run-as-system.ps1 -Stop"
