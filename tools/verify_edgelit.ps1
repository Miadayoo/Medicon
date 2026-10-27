[CmdletBinding()]
param([string]$CaptureDir = '')

# Checks scale=1, nine-row edgelit_idle/empty/no_effects fixtures on uniform
# white and black backgrounds. Generate them with capture_edgelit.ps1.
# RGB checks deliberately ignore PNG alpha (swap chains may not preserve it).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..'))
if (!$CaptureDir) { $CaptureDir = Join-Path $root 'out/edgelit' }

# Read pixels once. LockBits avoids millions of PowerShell GetPixel calls while
# still inspecting actual screenshot pixels through System.Drawing.
if (-not ('MediconEdgeLitPixels' -as [type])) {
    # Windows PowerShell uses System.Drawing; PowerShell 7 forwards Bitmap and
    # Rectangle to separate assemblies. Resolve both rather than name one DLL.
    $drawingReferences = @([System.Drawing.Bitmap].Assembly.Location, [System.Drawing.Rectangle].Assembly.Location) | Select-Object -Unique
    # .NET 10 moved the bitmap interfaces into additional Windows assemblies.
    # They are implementation dependencies of System.Drawing.Common, so include
    # them when present while retaining Windows PowerShell compatibility.
    $drawingDirectory = Split-Path ([System.Drawing.Bitmap].Assembly.Location)
    foreach ($assemblyName in @('System.Private.Windows.GdiPlus.dll', 'System.Private.Windows.Core.dll')) {
        $assemblyPath = Join-Path $drawingDirectory $assemblyName
        if (Test-Path -LiteralPath $assemblyPath) { $drawingReferences += $assemblyPath }
    }
    Add-Type -ReferencedAssemblies $drawingReferences @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public sealed class MediconEdgeLitPixels {
    public readonly int Width, Height;
    private readonly byte[] pixels;
    public MediconEdgeLitPixels(string path) {
        using (var image = new Bitmap(path)) {
            Width = image.Width; Height = image.Height;
            pixels = new byte[Width * Height * 4];
            var data = image.LockBits(new Rectangle(0, 0, Width, Height),
                ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try {
                for (int y = 0; y < Height; ++y)
                    Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride), pixels,
                        y * Width * 4, Width * 4);
            } finally { image.UnlockBits(data); }
        }
    }
    public int[] Rgb(int x, int y) {
        int i = (y * Width + x) * 4;
        return new int[] { pixels[i + 2], pixels[i + 1], pixels[i] };
    }
    public double Luma(int x, int y) {
        int i = (y * Width + x) * 4;
        return (pixels[i] + pixels[i + 1] + pixels[i + 2]) / 3.0;
    }
    public long CountDifferentFrom(int value, int tolerance,
        int x0, int y0, int x1, int y1) {
        long count = 0;
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
            int i = (y * Width + x) * 4;
            if (Math.Abs(pixels[i] - value) > tolerance ||
                Math.Abs(pixels[i + 1] - value) > tolerance ||
                Math.Abs(pixels[i + 2] - value) > tolerance) ++count;
        }
        return count;
    }
    public int MaxDelta(MediconEdgeLitPixels other, int x0, int y0, int x1, int y1) {
        int maximum = 0;
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
            int i = (y * Width + x) * 4;
            for (int c = 0; c < 3; ++c)
                maximum = Math.Max(maximum, Math.Abs(pixels[i + c] - other.pixels[i + c]));
        }
        return maximum;
    }
    public long CountChanged(MediconEdgeLitPixels other, int tolerance) {
        long count = 0;
        for (int i = 0; i < pixels.Length; i += 4) {
            if (Math.Abs(pixels[i] - other.pixels[i]) > tolerance ||
                Math.Abs(pixels[i + 1] - other.pixels[i + 1]) > tolerance ||
                Math.Abs(pixels[i + 2] - other.pixels[i + 2]) > tolerance) ++count;
        }
        return count;
    }
    public int CountBright(int x0, int y0, int x1, int y1, double threshold) {
        int count = 0;
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x)
            if (Luma(x, y) >= threshold) ++count;
        return count;
    }
    public int CountStableBright(MediconEdgeLitPixels other, int x0, int y0,
        int x1, int y1, double threshold, int tolerance) {
        int count = 0;
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
            if (Luma(x, y) < threshold) continue;
            int i = (y * Width + x) * 4;
            if (Math.Abs(pixels[i] - other.pixels[i]) <= tolerance &&
                Math.Abs(pixels[i + 1] - other.pixels[i + 1]) <= tolerance &&
                Math.Abs(pixels[i + 2] - other.pixels[i + 2]) <= tolerance) ++count;
        }
        return count;
    }
}
'@
}

$failures = 0
$checks = 0
function Check([bool]$Pass, [string]$Name, [string]$Detail = '') {
    $script:checks++
    if (!$Pass) { $script:failures++ }
    $status = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-Output ("{0} {1}{2}" -f $status, $Name, $(if ($Detail) { ": $Detail" } else { '' }))
}
function Read-Capture([string]$Name) {
    $path = Join-Path $CaptureDir "$Name.png"
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing capture: $path. Run tools/capture_edgelit.ps1 first." }
    return [MediconEdgeLitPixels]::new([IO.Path]::GetFullPath($path))
}
function Color-Delta($Actual, $Expected) {
    $largest = 0.0
    for ($c = 0; $c -lt 3; $c++) { $largest = [Math]::Max($largest, [Math]::Abs($Actual[$c] - $Expected[$c])) }
    return $largest
}

$images = @{}
foreach ($background in @('white', 'black')) {
    foreach ($state in @('idle', 'empty', 'no_effects')) {
        $key = "${background}_${state}"
        $images[$key] = Read-Capture $key
        if ($images[$key].Width -ne 1920 -or $images[$key].Height -ne 1080) {
            throw "$key must be 1920x1080 at ArrayList scale=1 (got $($images[$key].Width)x$($images[$key].Height))."
        }
    }
}
$width = 1920; $height = 1080; $top = 8; $rowHeight = 24; $rows = 9
$bottom = $top + $rowHeight * $rows
$right = $width - 9

foreach ($background in @('white', 'black')) {
    $idle = $images["${background}_idle"]
    $empty = $images["${background}_empty"]
    $plain = $images["${background}_no_effects"]
    $bg = if ($background -eq 'white') { 255 } else { 0 }
    $changed = $empty.CountDifferentFrom($bg, 0, 0, 0, $width, $height)
    Check ($changed -eq 0) "$background empty list leaves every background pixel unchanged" "$changed changed pixels"

    # Tube body is opaque. Its 3 columns, including the white core, must not
    # change when glow/shadow are disabled or the world background changes.
    $delta = $idle.MaxDelta($plain, $right, $top + 4, $right + 3, $bottom - 4)
    Check ($delta -le 1) "$background tube body and white core ignore glow/shadow" "maximum channel delta $delta"

    $gradientError = 0.0
    for ($row = 0; $row -lt $rows; $row++) {
        $y = $top + $row * $rowHeight + 12
        $t = ($y + 0.5 - $top) / ($bottom - $top)
        $expected = @((255 + (139 - 255) * $t), (79 + (107 - 79) * $t), (160 + (255 - 160) * $t))
        foreach ($x in @($right, ($right + 2))) {
            $gradientError = [Math]::Max($gradientError, (Color-Delta ($plain.Rgb($x, $y)) $expected))
        }
    }
    Check ($gradientError -le 2) "$background tube follows the specified pink-to-violet linear gradient" ("maximum error {0:F2}/255" -f $gradientError)
}
$crossBackgroundDelta = $images.white_idle.MaxDelta($images.black_idle, $right, $top + 4, $right + 3, $bottom - 4)
Check ($crossBackgroundDelta -le 1) 'opaque tube has the same color over black and white' "maximum channel delta $crossBackgroundDelta"

$plainBlack = $images.black_no_effects
$tubeColumns = @()
for ($x = $right - 3; $x -lt $right + 6; $x++) {
    if ($plainBlack.Luma($x, 116) -gt 60) { $tubeColumns += $x }
}
Check (($tubeColumns -join ',') -eq (@($right, ($right + 1), ($right + 2)) -join ',')) 'tube occupies exactly three physical pixel columns' ($tubeColumns -join ',')
$darkTubePixels = 0
for ($y = $top + 4; $y -lt $bottom - 4; $y++) {
    if ($plainBlack.Luma($right, $y) -lt 80) { $darkTubePixels++ }
}
Check ($darkTubePixels -eq 0) 'tube remains continuous through all nine rows' "$darkTubePixels dark interruptions"

# Detect each plate's left edge in a text-free band near the row top. No exact
# glyph advance is hard-coded; this also catches missing/collapsed rows.
$leftEdges = @()
$baseError = 0.0; $missingTextRows = 0; $unstableTextRows = 0
for ($row = 0; $row -lt $rows; $row++) {
    $y = $top + $row * $rowHeight + 3
    $left = -1
    for ($x = $right - 500; $x -lt $right; $x++) {
        if ($images.white_no_effects.Luma($x, $y) -lt 180) { $left = $x; break }
    }
    if ($left -lt 0 -or $right - $left -lt 30) {
        Check $false "row $row has a measurable dark plate"
        continue
    }
    $leftEdges += $left
    foreach ($background in @('white', 'black')) {
        $bg = if ($background -eq 'white') { 255 } else { 0 }
        $expectedBase = @(($bg * .34 + 10 * .66), ($bg * .34 + 11 * .66), ($bg * .34 + 17 * .66))
        $actual = $images["${background}_no_effects"].Rgb(($left + 6), $y)
        $baseError = [Math]::Max($baseError, (Color-Delta $actual $expectedBase))
    }
    $textTop = $top + $row * $rowHeight + 4
    $textBottom = $top + ($row + 1) * $rowHeight - 3
    $bright = $plainBlack.CountBright(($left + 10), $textTop, ($right - 11), $textBottom, 160)
    if ($bright -lt 8) { $missingTextRows++ }
    # Only fully covered glyph interiors are expected to remain equal: their
    # antialiased edges legitimately blend with a different illuminated plate.
    $stable = $plainBlack.CountStableBright($images.black_idle, ($left + 10), $textTop, ($right - 11), $textBottom, 210, 3)
    if ($stable -lt 2) { $unstableTextRows++ }
}
Check ($leftEdges.Count -eq $rows) 'all nine 24px rows retain their plate geometry with zero effects' ($leftEdges -join ',')
$stepped = $leftEdges.Count -eq $rows
for ($i = 1; $i -lt $leftEdges.Count; $i++) { if ($leftEdges[$i] -lt $leftEdges[$i - 1]) { $stepped = $false } }
Check $stepped 'plate widths descend to form a right-aligned staircase'
Check ($baseError -le 2) 'zero-effects plate base is rgba(10,11,17,0.66) over both backgrounds' ("maximum channel error {0:F2}/255" -f $baseError)
Check ($missingTextRows -eq 0) 'zero effects preserves readable foreground glyphs in every row' "$missingTextRows rows lack foreground ink"
Check ($unstableTextRows -eq 0) 'disabling effects preserves sharp name glyph body colors' "$unstableTextRows rows lack matching glyph interiors"

if ($leftEdges.Count -eq $rows) {
    $left = $leftEdges[0]
    $outside = $images.white_no_effects.CountDifferentFrom(255, 0, 0, 0, $left - 2, $height) +
        $plainBlack.CountDifferentFrom(0, 0, ($right + 3), 0, $width, $height)
    Check ($outside -eq 0) 'zero effects removes the shadow and glow outside the geometry' "$outside unexpected outside pixels"

    # At the long top row's left edge the tube bloom is more than 100px away.
    # A sigma=11 shadow should span many luminance levels without a hard edge.
    $profile = @()
    for ($x = $left - 60; $x -le $left - 3; $x++) {
        $profile += 255 - $images.white_idle.Luma($x, ($top + 12))
    }
    $maxJump = 0.0; $reversals = 0
    for ($i = 1; $i -lt $profile.Count; $i++) {
        $maxJump = [Math]::Max($maxJump, [Math]::Abs($profile[$i] - $profile[$i - 1]))
        if ($profile[$i] -lt $profile[$i - 1] - 2) { $reversals++ }
    }
    $levels = @($profile | ForEach-Object { [Math]::Round($_) } | Select-Object -Unique).Count
    Check ($profile[0] -le 3 -and $profile[-1] -ge 8 -and $maxJump -le 12 -and $levels -ge 8 -and $reversals -eq 0) `
        'cast-shadow penumbra fades smoothly into the white background' `
        ("far={0:F1}, near={1:F1}, largest 1px step={2:F1}, levels={3}, reversals={4}" -f $profile[0], $profile[-1], $maxJump, $levels, $reversals)
}
$outsideTubeGlow = $images.black_idle.Luma(($width - 3), 116)
Check ($outsideTubeGlow -ge 5) 'night glow visibly spreads outside the tube' ("outside-tube intensity {0:F1}/255" -f $outsideTubeGlow)

# If the capture batch also contains animation series, reject frozen frames.
# Their day/960x640 backgrounds differ from the mandatory white/black fixtures,
# so comparisons stay within each series rather than against an idle fixture.
foreach ($series in @('on', 'off', 'mode')) {
    if (!(Test-Path -LiteralPath (Join-Path $CaptureDir "${series}_t060.png"))) { continue }
    $early = Read-Capture "${series}_t060"
    $late = Read-Capture "${series}_done"
    $sameSize = $early.Width -eq $late.Width -and $early.Height -eq $late.Height
    Check $sameSize "$series animation captures use matching dimensions"
    if ($sameSize) {
        $changed = $early.CountChanged($late, 5)
        Check ($changed -ge 20) "$series animation changes visible pixels from 60ms to settled" "$changed changed pixels"
    }
}

Write-Output "$checks checks, $failures failure(s)"
exit $(if ($failures) { 1 } else { 0 })
