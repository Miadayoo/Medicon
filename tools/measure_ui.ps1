[CmdletBinding()]
param(
    [string]$Preview = 'out/build/local/bin/MediconPreview.exe',
    [string]$Output = 'out/performance/current',
    [ValidateRange(1, 20)][int]$Runs = 3,
    [ValidateRange(1, 100000)][int]$Frames = 600
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$previewPath = if ([IO.Path]::IsPathRooted($Preview)) { [IO.Path]::GetFullPath($Preview) }
    else { [IO.Path]::GetFullPath((Join-Path $projectRoot $Preview)) }
$outputPath = if ([IO.Path]::IsPathRooted($Output)) { [IO.Path]::GetFullPath($Output) }
    else { [IO.Path]::GetFullPath((Join-Path $projectRoot $Output)) }
if (!(Test-Path -LiteralPath $previewPath)) { throw "Preview not found: $previewPath" }
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

# Run serially: concurrent GPU workloads invalidate comparisons. Hidden modes
# use demo modules and never load or write the user's config.
$cases = @(
    @{ Name = 'open_1080'; Size = '1920x1080'; Args = @('--scenario', 'open') },
    @{ Name = 'settings_1080'; Size = '1920x1080'; Args = @('--scenario', 'settings') },
    @{ Name = 'hud_1080'; Size = '1920x1080'; Args = @('--scenario', 'edgelit_idle', '--bg', 'day') },
    @{ Name = 'hud_4k'; Size = '3840x2160'; Args = @('--scenario', 'edgelit_idle', '--bg', 'day') },
    @{ Name = 'hud_half'; Size = '1920x1080'; Args = @('--scenario', 'edgelit_idle', '--bg', 'white', '--arraylist-scale', '0.5') },
    @{ Name = 'hud_scale_cycle'; Size = '1920x1080'; Args = @('--scenario', 'edgelit_scale_cycle', '--bg', 'white') },
    @{ Name = 'menu_1080'; Size = '1920x1080'; Args = @('--main-menu-time', '1.5') },
    @{ Name = 'splash_1080'; Size = '1920x1080'; Args = @('--splash-time', '1.3') }
)

Push-Location $projectRoot
try {
    for ($run = 1; $run -le $Runs; ++$run) {
        foreach ($case in $cases) {
            $stem = Join-Path $outputPath ($case.Name + '-' + $run)
            $arguments = @('--size', $case.Size, '--dpi', '96', '--fixed-dt', '0.016666667',
                '--gpu-timing', '--gpu-frames', $Frames, '--log', ('"' + $stem + '.log"')) + $case.Args
            if ($run -eq 1) { $arguments += @('--screenshot', ('"' + $stem + '.png"')) }
            $process = Start-Process -FilePath $previewPath -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
            if ($process.ExitCode -ne 0) { throw "Measurement failed: $($case.Name) (exit $($process.ExitCode))" }
            $measurements = Get-Content -LiteralPath ($stem + '.log') | Where-Object { $_ -match '^(cpu|gpu)_timing ' }
            if ($measurements.Count -ne 2) { throw "Missing timing samples: $stem" }
            Write-Output "$($case.Name) run=$run"
            $measurements | Write-Output
        }
    }
} finally {
    Pop-Location
}
