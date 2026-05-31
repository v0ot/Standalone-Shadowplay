#requires -Version 5.1
# extract-installer.ps1
# Unpacks the NVIDIA App installer into ..\extracted\ without running install.
# Walks the resulting tree, recursively expanding nested archives (.7z, .cab, .nvi self-extractors).
# Emits inventory.csv with every PE file's name, size, SHA256, and version info.

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$InstallerPath,

    [string]$Out = (Join-Path $PSScriptRoot '..\extracted'),

    [string]$SevenZip = 'C:\Program Files\7-Zip\7z.exe'
)

$ErrorActionPreference = 'Stop'
$Out = [System.IO.Path]::GetFullPath($Out)

if (-not (Test-Path $InstallerPath)) {
    throw "Installer not found: $InstallerPath"
}
if (-not (Test-Path $SevenZip)) {
    throw "7-Zip not found at $SevenZip"
}

if (Test-Path $Out) {
    Write-Host "Wiping prior extract at $Out"
    Remove-Item -Recurse -Force $Out
}
New-Item -ItemType Directory -Path $Out | Out-Null

Write-Host "Stage 1: top-level extract of $InstallerPath -> $Out"
& $SevenZip x "-o$Out" "$InstallerPath" -y -bso0 -bsp0 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "7z stage-1 failed (exit $LASTEXITCODE)" }

# Stage 2: walk the tree and expand any inner archives we recognize.
# NVIDIA wraps a 7z stub around a payload tree that sometimes contains
# .7z, .cab, or further self-extractors (esp. for sub-components).
$expandable = @('.7z', '.cab', '.zip')
$rounds = 0
do {
    $rounds++
    $found = $false
    $candidates = Get-ChildItem -Path $Out -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $expandable -contains $_.Extension.ToLower() }
    foreach ($c in $candidates) {
        $marker = "$($c.FullName).extracted"
        if (Test-Path $marker) { continue }
        $dest = Join-Path $c.DirectoryName ([IO.Path]::GetFileNameWithoutExtension($c.Name) + '__unpack')
        if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }
        Write-Host "  stage-$rounds expand: $($c.Name) -> $dest"
        & $SevenZip x "-o$dest" "$($c.FullName)" -y -bso0 -bsp0 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Set-Content -Path $marker -Value (Get-Date).ToString('o')
            $found = $true
        } else {
            Write-Host "    (skipped, 7z exit $LASTEXITCODE)"
        }
    }
} while ($found -and $rounds -lt 8)

Write-Host "Stage 3: inventory PE files"
$inv = @()
$peFiles = Get-ChildItem -Path $Out -Recurse -File -Include *.dll, *.exe, *.sys -ErrorAction SilentlyContinue
foreach ($f in $peFiles) {
    $vi = $null
    try { $vi = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($f.FullName) } catch {}
    $sha = $null
    try { $sha = (Get-FileHash -Algorithm SHA256 -Path $f.FullName).Hash } catch {}
    $inv += [pscustomobject]@{
        Path        = $f.FullName.Substring($Out.Length).TrimStart('\','/')
        Name        = $f.Name
        Size        = $f.Length
        SHA256      = $sha
        FileVersion = if ($vi) { $vi.FileVersion } else { '' }
        ProductName = if ($vi) { $vi.ProductName } else { '' }
        Description = if ($vi) { $vi.FileDescription } else { '' }
    }
}

$invPath = Join-Path $Out 'inventory.csv'
$inv | Sort-Object Path | Export-Csv -NoTypeInformation -Path $invPath
Write-Host ""
Write-Host "Done. $($inv.Count) PE files indexed at $invPath"
Write-Host ""

# Quick Shadowplay-suspect summary
$pat = 'nvsp|nvbackend|shadow|share|nvcontainer|capture|replay|nvaudcap|overlay|hotkey|fbc|enc'
Write-Host "ShadowPlay-suspect files (regex: $pat):"
$inv | Where-Object { $_.Name -match $pat } |
    Sort-Object Name |
    Format-Table Name, Size, Description, FileVersion -AutoSize
