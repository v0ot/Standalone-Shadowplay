#requires -Version 5.1
# seed-registry.ps1
#
# Seeds the two registry values that ShadowPlayServicePlugin::ShadowPlayServicePlugin
# (in nvspserviceplugin64.dll) expects to find before it will initialise on this machine.
#
# ShadowPlayCfg2 (REG_BINARY, 16 bytes)
#   On read, each byte is XOR'd with 0xA6, then compared against the literal
#       qword[0] = 0x4EF336A21BC9EF0D
#       qword[1] = 0x5E97883A3AF7F9B2
#   so we must STORE qword XOR 0xA6A6A6A6A6A6A6A6 :
#       qword[0] ^= 0xA6A6A6A6A6A6A6A6 = 0xE8559004BD6F49AB
#       qword[1] ^= 0xA6A6A6A6A6A6A6A6 = 0xF8312E9C9C515F14
#
# ShadowPlayCfg3 (REG_DWORD)
#   This is the client-id field that nvspapi64!CreateShadowPlayApiInterface accepts
#   (range 3..10). The hardcoded default in ipccommon64 when Cfg3 is missing is 3.
#
# Reference: IDA decompile of ipccommon64.dll!sub_18001FFA0 and
#            nvspserviceplugin64.dll's ShadowPlayServicePlugin ctor.
#
# Requires admin privileges (writing under HKLM).

[CmdletBinding()]
param(
    [uint32]$ClientId = 3
)

$ErrorActionPreference = 'Stop'

$keyPath = 'HKLM:\SOFTWARE\NVIDIA Corporation\Global\NvApp\ShadowPlay'

# Build Cfg2: target qwords XOR'd with 0xA6A6...A6 (little-endian byte order)
$target = [byte[]](0x0D,0xEF,0xC9,0x1B,0xA2,0x36,0xF3,0x4E,  # 0x4EF336A21BC9EF0D LE
                   0xB2,0xF9,0xF7,0x3A,0x3A,0x88,0x97,0x5E)  # 0x5E97883A3AF7F9B2 LE
$cfg2  = [byte[]]::new(16)
for ($i = 0; $i -lt 16; $i++) { $cfg2[$i] = $target[$i] -bxor 0xA6 }

Write-Host "Computed ShadowPlayCfg2 (REG_BINARY, 16 bytes):"
Write-Host ("  " + ($cfg2 | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
Write-Host ""

# Ensure the key exists.
if (-not (Test-Path $keyPath)) {
    Write-Host "Creating registry key: $keyPath"
    try {
        New-Item -Path $keyPath -Force -ErrorAction Stop | Out-Null
    } catch {
        Write-Host "[!] $_"
        Write-Host "[!] Re-run this script from an elevated PowerShell. (Run as Administrator.)"
        exit 2
    }
} else {
    Write-Host "Key already exists: $keyPath"
}

try {
    New-ItemProperty -Path $keyPath -Name 'ShadowPlayCfg2' -PropertyType Binary -Value $cfg2 -Force | Out-Null
    New-ItemProperty -Path $keyPath -Name 'ShadowPlayCfg3' -PropertyType DWord  -Value $ClientId -Force | Out-Null
} catch {
    Write-Host "[!] Failed to write registry values: $_"
    Write-Host "[!] Re-run this script as Administrator."
    exit 2
}

Write-Host ""
Write-Host "Verifying:"
Get-ItemProperty -Path $keyPath | Format-List ShadowPlayCfg2, ShadowPlayCfg3
Write-Host "[+] Seed complete."
