[CmdletBinding()]
param(
    [string]$Preview = '',
    [string]$OutDir = '',
    [string[]]$Scenarios = @('closed', 'open', 'collapsed', 'settings', 'dropdown', 'colorpicker', 'scroll',
                             'arraylist_on', 'arraylist_off', 'shadow_on', 'shadow_off', 'shadow_profile'),
    [string[]]$Sizes = @('1920x1080@96')
)
# Renders Preview scenarios offscreen and saves docs/screenshots/<scenario>_<W>x<H>_<dpi>.png.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..'))
if (!$Preview) { $Preview = Join-Path $root 'out/build/local/bin/MediconPreview.exe' }
if (!$OutDir) { $OutDir = Join-Path $root 'docs/screenshots' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($entry in $Sizes) {
    $size, $dpi = $entry.Split('@')
    foreach ($scenario in $Scenarios) {
        $bg = if ($scenario -like 'shadow_*') { 'white' } else { 'default' }
        $out = Join-Path $OutDir "${scenario}_${size}_${dpi}.png"
        & $Preview --scenario $scenario --size $size --dpi $dpi --bg $bg --fixed-dt 0.0166667 --screenshot $out |
            Where-Object { $_ -match '^(panel|screenshot|capture|dropdown|color|shadow)' }
        if ($LASTEXITCODE -ne 0) { throw "Scenario $scenario at $entry failed" }
    }
}
