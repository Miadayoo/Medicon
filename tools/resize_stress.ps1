[CmdletBinding()]
param([int]$Iterations = 200, [string]$Preview = '')
# Resizes a live Preview window continuously (spec 9.3) and reports memory and
# the Preview's own resource counters. Uses a temporary LOCALAPPDATA so the
# user's configuration is never touched.
$ErrorActionPreference = 'Stop'
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int cx, uint flags);
}
"@
$root = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..'))
if (!$Preview) { $Preview = Join-Path $root 'out/build/local/bin/MediconPreview.exe' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ("medicon-resize-" + [Guid]::NewGuid())
New-Item -ItemType Directory $temp | Out-Null
$log = Join-Path $temp 'preview.log'
$env:LOCALAPPDATA = $temp
$process = Start-Process -FilePath $Preview -ArgumentList @('--demo', '--log', "`"$log`"") -PassThru
Start-Sleep -Seconds 2
$process.Refresh()
$handle = $process.MainWindowHandle
$samples = @()
for ($i = 0; $i -lt $Iterations; $i++) {
    $w = 700 + (($i * 97) % 1900)
    $h = 500 + (($i * 53) % 1000)
    [Win]::SetWindowPos($handle, [IntPtr]::Zero, 20, 20, $w, $h, 0x0014) | Out-Null # SWP_NOZORDER|SWP_NOACTIVATE
    Start-Sleep -Milliseconds 25
    if ($i % 50 -eq 0) { $process.Refresh(); $samples += [int]($process.PrivateMemorySize64 / 1MB) }
}
Start-Sleep -Milliseconds 500
$process.Refresh()
$samples += [int]($process.PrivateMemorySize64 / 1MB)
$alive = !$process.HasExited
$process.CloseMainWindow() | Out-Null
if (!$process.WaitForExit(10000)) { $process.Kill(); throw 'Preview did not exit' }
Write-Output ("alive after resizing: {0}; exit code {1}" -f $alive, $process.ExitCode)
Write-Output ("private MB every 50 resizes: " + ($samples -join ', '))
Get-Content $log | Where-Object { $_ -match '^(shadow|Fonts)' }
Remove-Item -Recurse -Force $temp
