[CmdletBinding()]
param([string]$Output = 'out/edgelit')
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$preview = Join-Path $projectRoot 'out/build/local/bin/MediconPreview.exe'
if (!(Test-Path -LiteralPath $preview)) { throw 'Build MediconPreview first with tools/build.ps1.' }
$outputPath = [IO.Path]::GetFullPath((Join-Path $projectRoot $Output))
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

function Capture([string]$Name, [string]$Scenario, [string]$Background, [string]$Size = '1920x1080', [string]$Scale = '1') {
    $destination = Join-Path $outputPath ($Name + '.png')
    $captureArgs = @('--demo', '--size', $Size, '--dpi', '96', '--arraylist-scale', $Scale,
        '--bg', $Background, '--scenario', $Scenario, '--screenshot', ('"' + $destination + '"'))
    $process = Start-Process -FilePath $preview -ArgumentList $captureArgs -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "Preview capture failed: $Name (exit $($process.ExitCode))" }
    Write-Output "Captured $Name"
}

# The supplied reference is 2x; keep that comparison separate from physical-pixel probes.
Capture 'day' 'edgelit_reference' 'day' '960x640' '2'
Capture 'night' 'edgelit_reference' 'night' '960x640' '2'
foreach ($background in @('white', 'black')) {
    Capture ($background + '_idle') 'edgelit_idle' $background
    Capture ($background + '_empty') 'edgelit_empty' $background
    Capture ($background + '_no_effects') 'edgelit_no_effects' $background
}
foreach ($scenario in @('on', 'off', 'mode', 'no_pulse')) {
    Capture $scenario ('edgelit_' + $scenario) 'day' '960x640'
}
Capture 'fractional_scale' 'edgelit_scale' 'day' '960x640'
Capture 'single' 'edgelit_single' 'white'
