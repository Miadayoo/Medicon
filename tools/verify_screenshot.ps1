[CmdletBinding()]
param([string]$Open = '', [string]$Collapsed = '')
# Pixel checks on 1920x1080 / 96 DPI Preview screenshots: category gradients,
# full-width module states, square borderless panels, and text centring.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..'))
if (!$Open) { $Open = Join-Path $root 'docs/screenshots/open_1920x1080_96.png' }
if (!$Collapsed) { $Collapsed = Join-Path $root 'docs/screenshots/collapsed_1920x1080_96.png' }
$themeSource = Get-Content -Raw -LiteralPath (Join-Path $root 'include/medicon/gui/Theme.hpp')
$failures = 0

function Get-Metric([string]$name) {
    $match = [regex]::Match($themeSource, ('\b' + [regex]::Escape($name) + '\s*=\s*([0-9.]+)F\s*;'))
    if (!$match.Success) { throw "Theme metric '$name' was not found" }
    return [double]::Parse($match.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Check-Color($bmp, [int]$x, [int]$y, [int[]]$expected, [string]$name) {
    $c = $bmp.GetPixel($x, $y)
    $ok = ([Math]::Abs($c.R - $expected[0]) -le 1) -and
          ([Math]::Abs($c.G - $expected[1]) -le 1) -and
          ([Math]::Abs($c.B - $expected[2]) -le 1)
    $wanted = '{0:X2}{1:X2}{2:X2}' -f $expected[0], $expected[1], $expected[2]
    $actual = '{0:X2}{1:X2}{2:X2}' -f $c.R, $c.G, $c.B
    Write-Output ("{0} {1} at ({2},{3}): expected #{4} got #{5}" -f
        ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name, $x, $y, $wanted, $actual)
    if (!$ok) { $script:failures++ }
}

function Get-GradientColor([int[]]$accent, [double]$leftFactor, [double]$rightFactor, [int]$offset, [int]$width) {
    $t = ($offset + 0.5) / $width
    return @($accent | ForEach-Object {
        $left = [Math]::Round($_ * $leftFactor, [MidpointRounding]::AwayFromZero)
        $right = [Math]::Round($_ * $rightFactor, [MidpointRounding]::AwayFromZero)
        [int][Math]::Round($left + ($right - $left) * $t, [MidpointRounding]::AwayFromZero)
    })
}

function Get-PixelColor($bmp, [int]$x, [int]$y) {
    $c = $bmp.GetPixel($x, $y)
    return @([int]$c.R, [int]$c.G, [int]$c.B)
}

# Distance from the row top to the top of the 'H' ink and from its bottom to the row bottom.
function Check-Centering($bmp, [int]$x0, [int]$x1, [int]$top, [int]$bottom, [string]$name) {
    $bg = $bmp.GetPixel($x1 + 2, $top + 2)
    $inkTop = -1; $inkBottom = -1
    for ($y = $top; $y -lt $bottom; $y++) {
        for ($x = $x0; $x -le $x1; $x++) {
            $c = $bmp.GetPixel($x, $y)
            if (([Math]::Abs($c.R - $bg.R) + [Math]::Abs($c.G - $bg.G) + [Math]::Abs($c.B - $bg.B)) -gt 90) {
                if ($inkTop -lt 0) { $inkTop = $y }
                $inkBottom = $y
            }
        }
    }
    $above = $inkTop - $top
    $below = $bottom - 1 - $inkBottom
    $ok = $inkTop -ge 0 -and [Math]::Abs($above - $below) -le 1
    Write-Output ("{0} {1}: 'H' ink rows {2}..{3} in row {4}..{5}: gap above {6}, below {7}" -f
        ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name, $inkTop, $inkBottom, $top, ($bottom - 1), $above, $below)
    if (!$ok) { $script:failures++ }
}

$startX = [int](Get-Metric 'start_x')
$startY = [int](Get-Metric 'start_y')
$panelWidth = [int](Get-Metric 'panel_width')
$panelGap = [int](Get-Metric 'panel_gap')
$headerHeight = [int](Get-Metric 'header_height')
$moduleHeight = [int](Get-Metric 'module_height')
$moduleTextX = [int](Get-Metric 'module_text_x')
$bodyTop = $startY + $headerHeight + [int](Get-Metric 'separator')
$accents = @(@(153, 22, 255), @(213, 27, 232), @(240, 22, 183), @(255, 40, 107), @(255, 64, 40))
$categoryNames = @('Visual', 'Movement', 'Player', 'Misc', 'Client')

$openBmp = [System.Drawing.Bitmap]::FromFile($Open)
$collapsedBmp = [System.Drawing.Bitmap]::FromFile($Collapsed)
try {
    foreach ($bmp in @($openBmp, $collapsedBmp)) {
        if ($bmp.Width -ne 1920 -or $bmp.Height -ne 1080) {
            throw 'Screenshot verification requires 1920x1080 captures at 96 DPI and GUI Scale 1.'
        }
    }
    for ($i = 0; $i -lt $accents.Count; $i++) {
        $x = $startX + $i * ($panelWidth + $panelGap)
        # The top corners retain the ideal gradient and catch borders or rounded corners.
        foreach ($offset in @(0, ($panelWidth - 1))) {
            $expected = Get-GradientColor $accents[$i] 0.88 1.0 $offset $panelWidth
            Check-Color $openBmp ($x + $offset) $startY $expected ($categoryNames[$i] + ' header top edge')
            # A later panel's Gaussian shadow can darken the neighbouring panel's right edge.
            # Compare the bottom edge to the immediately preceding row at the same X to
            # verify border continuity while retaining the one-channel-value tolerance.
            $edgeColor = Get-PixelColor $collapsedBmp ($x + $offset) ($startY + $headerHeight - 2)
            Check-Color $collapsedBmp ($x + $offset) ($startY + $headerHeight - 1) $edgeColor ($categoryNames[$i] + ' collapsed borderless bottom edge')
        }
        # Eight pixels inside the panel are beyond the visible neighbouring shadow.
        # Check the interpolated hue there instead of increasing the colour tolerance.
        foreach ($offset in @(8, ($panelWidth - 9))) {
            $expected = Get-GradientColor $accents[$i] 0.88 1.0 $offset $panelWidth
            Check-Color $collapsedBmp ($x + $offset) ($startY + $headerHeight - 1) $expected ($categoryNames[$i] + ' collapsed header gradient')
        }
    }

    # Fullbright is enabled in the open scenario. Verify its fill across the panel,
    # then the shadowed rightmost column against the adjacent pixel of the same row.
    foreach ($offset in @(0, ($panelWidth - 9))) {
        $expected = Get-GradientColor $accents[0] 0.72 0.84 $offset $panelWidth
        Check-Color $openBmp ($startX + $offset) ($bodyTop + [int]($moduleHeight / 2)) $expected 'full-width enabled module'
    }
    $enabledY = $bodyTop + [int]($moduleHeight / 2)
    $enabledEdgeColor = Get-PixelColor $openBmp ($startX + $panelWidth - 2) $enabledY
    Check-Color $openBmp ($startX + $panelWidth - 1) $enabledY $enabledEdgeColor 'enabled row borderless right edge'
    foreach ($offset in @(0, ($panelWidth - 1))) {
        Check-Color $openBmp ($startX + $offset) ($bodyTop + $moduleHeight + [int]($moduleHeight / 2)) @(54, 54, 54) 'flat disabled module'
    }
    # The enabled gradient starts directly below the header, with no separator or accent stripe.
    Check-Color $openBmp ($startX + 2) $bodyTop (Get-GradientColor $accents[0] 0.72 0.84 2 $panelWidth) 'first module top edge'

    $demoSource = Get-Content -Raw -LiteralPath (Join-Path $root 'src/preview/DemoModules.cpp')
    $visualRows = [regex]::Matches($demoSource, 'add\("[^"]+",\s*"[^"]+",\s*C::Visual').Count
    $panelBottom = $bodyTop + $visualRows * $moduleHeight + [int](Get-Metric 'bottom_padding')
    Check-Color $openBmp ($startX + [int]($panelWidth / 2)) ($panelBottom - 1) @(51, 51, 51) 'panel bottom padding'

    $textX = $startX + $moduleTextX
    $hitColorTop = $bodyTop + 6 * $moduleHeight
    Check-Centering $openBmp $textX ($textX + 10) $hitColorTop ($hitColorTop + $moduleHeight) 'module row Hit Color'
    $playerTextX = $startX + 2 * ($panelWidth + $panelGap) + $moduleTextX
    $hitDelayTop = $bodyTop + 2 * $moduleHeight
    Check-Centering $openBmp $playerTextX ($playerTextX + 10) $hitDelayTop ($hitDelayTop + $moduleHeight) 'module row Hit Delay Fix'
} finally {
    $openBmp.Dispose()
    $collapsedBmp.Dispose()
}
Write-Output "$failures failure(s)"
exit $failures
