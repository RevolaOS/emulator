<#
.SYNOPSIS
    Dumps the firmware partition of a connected iPod (PortalPlayer generations).

.DESCRIPTION
    An iPod keeps its firmware in an unpartitioned area at the start of the
    disk, before the FAT32 data partition. On a Rockbox'd iPod that area holds
    both the Rockbox bootloader and Apple's original RetailOS image, neither of
    which is encrypted on PortalPlayer devices.

    This reads that area and nothing else. The disk is opened read-only, so the
    iPod is never written to; it can stay plugged in and mounted.

    The target disk is found by looking for a drive whose model reports an
    Apple iPod, rather than by hardcoding a drive number, because the numbering
    changes as devices are plugged in and out. The script refuses to run if it
    cannot find exactly one, or if the candidate looks like a system disk.

.PARAMETER Output
    Where to write the image. Defaults to assets/ipod-video-fw.bin next to the
    repository.

.EXAMPLE
    Right-click the Start menu, pick "Terminal (Administrator)", then:
    powershell -ExecutionPolicy Bypass -File C:\Desarrollo\qemu-ios\tools\ipod_video\dump-firmware.ps1
#>

[CmdletBinding()]
param(
    [string]$Output = "C:\Desarrollo\qemu-ios\assets\ipod-video-fw.bin"
)

$ErrorActionPreference = "Stop"

function Fail($msg) {
    Write-Host ""
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# Raw access to \\.\PhysicalDriveN needs elevation; there is no way around it.
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "this must be run as administrator (right-click the Start menu -> Terminal (Admin))"
}

Write-Host "Looking for the iPod..." -ForegroundColor Cyan

$candidates = @(Get-CimInstance Win32_DiskDrive |
    Where-Object { $_.Model -match 'iPod' -or $_.PNPDeviceID -match 'VEN_APPLE' })

if ($candidates.Count -eq 0) { Fail "cannot find any disk that is an iPod. Is it connected and mounted?" }
if ($candidates.Count -gt 1) {
    Write-Host "There is more than one candidate:"
    $candidates | ForEach-Object { Write-Host ("  {0}  {1}  {2:N1} GB" -f $_.DeviceID, $_.Model, ($_.Size / 1GB)) }
    Fail "disconnect the ones that do not belong and try again"
}

$disk = $candidates[0]
$diskNumber = $disk.Index

# Paranoia: never touch the disk Windows booted from.
$systemDisk = (Get-Partition -DriveLetter $env:SystemDrive.TrimEnd(':') -ErrorAction SilentlyContinue).DiskNumber
if ($null -ne $systemDisk -and $systemDisk -eq $diskNumber) {
    Fail "the detected disk ($diskNumber) is the system disk. Aborting."
}

Write-Host ("  {0}" -f $disk.DeviceID)
Write-Host ("  model    : {0}" -f $disk.Model)
Write-Host ("  size     : {0:N2} GB" -f ($disk.Size / 1GB))
Write-Host ("  serial   : {0}" -f $disk.SerialNumber)

# The firmware area runs from sector 0 up to wherever the first partition
# starts, so its size is that partition's offset.
$part = Get-Partition -DiskNumber $diskNumber -ErrorAction SilentlyContinue |
        Sort-Object Offset | Select-Object -First 1
if ($null -eq $part) { Fail "the disk has no partitions; cannot work out where the firmware ends" }

$len = [int64]$part.Offset
if ($len -le 0)          { Fail "invalid first partition offset ($len)" }
if ($len % 512 -ne 0)    { Fail "the offset ($len) is not sector-aligned; cannot read it raw" }
if ($len -gt 512MB)      { Fail "the offset ($len) is suspiciously large; aborting just in case" }

Write-Host ""
Write-Host ("Firmware partition: {0} bytes ({1:N2} MB)" -f $len, ($len / 1MB)) -ForegroundColor Cyan
Write-Host ("Destination       : {0}" -f $Output)
Write-Host ""

$dir = Split-Path -Parent $Output
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

$in = $null; $out = $null
try {
    # Read-only share so Windows keeping the volume mounted is not a problem.
    $in  = New-Object System.IO.FileStream("\\.\PhysicalDrive$diskNumber",
                                           [System.IO.FileMode]::Open,
                                           [System.IO.FileAccess]::Read,
                                           [System.IO.FileShare]::ReadWrite)
    $out = [System.IO.File]::Create($Output)

    $buf  = New-Object byte[] (1MB)
    $done = [int64]0
    $lastPct = -1

    while ($done -lt $len) {
        $want = [Math]::Min([int64]$buf.Length, $len - $done)
        $n = $in.Read($buf, 0, [int]$want)
        if ($n -le 0) { break }
        $out.Write($buf, 0, $n)
        $done += $n

        $pct = [int](100 * $done / $len)
        if ($pct -ne $lastPct) {
            Write-Progress -Activity "Dumping the firmware partition" -PercentComplete $pct `
                           -Status ("{0:N1} / {1:N1} MB" -f ($done / 1MB), ($len / 1MB))
            $lastPct = $pct
        }
    }
}
finally {
    if ($out) { $out.Close() }
    if ($in)  { $in.Close() }
    Write-Progress -Activity "Dumping the firmware partition" -Completed
}

Write-Host ""
if ($done -ne $len) {
    Write-Host ("WARNING: read {0} of {1} bytes" -f $done, $len) -ForegroundColor Yellow
} else {
    Write-Host ("OK: {0} bytes -> {1}" -f $done, $Output) -ForegroundColor Green
}

# A quick sanity check so a bad dump is obvious right away: every iPod firmware
# image starts with Apple's ASCII-art copyright banner and carries the "]ih["
# directory magic at 0x100.
$head = New-Object byte[] 260
$fs = [System.IO.File]::OpenRead($Output)
try { [void]$fs.Read($head, 0, $head.Length) } finally { $fs.Close() }

$banner = [System.Text.Encoding]::ASCII.GetString($head, 0, 8)
$magic  = [System.Text.Encoding]::ASCII.GetString($head, 256, 4)

Write-Host ""
Write-Host ("First bytes    : '{0}'" -f $banner)
Write-Host ("Magic at 0x100 : '{0}'" -f $magic)
if ($magic -eq ']ih[') {
    Write-Host "Looks like a valid iPod firmware partition." -ForegroundColor Green
} else {
    Write-Host "No ']ih[' magic at 0x100. The dump may not be what we expect." -ForegroundColor Yellow
}
