[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceDirectory,
    [ValidatePattern('^[a-z0-9][a-z0-9_-]{0,63}$')][string]$ModelId = 'neon-t-pose',
    [string]$DataDirectory = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Medicon')
)

$ErrorActionPreference = 'Stop'
$source = (Get-Item -LiteralPath $SourceDirectory).FullName
if (!(Test-Path -LiteralPath $source -PathType Container)) { throw 'SourceDirectory must be a directory.' }
$target = [IO.Path]::GetFullPath((Join-Path $DataDirectory "models/$ModelId"))
$separator = [IO.Path]::DirectorySeparatorChar
$sourcePrefix = $source.TrimEnd($separator) + $separator
$targetPrefix = $target.TrimEnd($separator) + $separator
if ($target.Equals($source, [StringComparison]::OrdinalIgnoreCase) -or
    $target.StartsWith($sourcePrefix, [StringComparison]::OrdinalIgnoreCase) -or
    $source.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Source and destination directories must not overlap.'
}

# Inventory before creating the destination. Do not follow linked asset trees.
$items = @(Get-ChildItem -LiteralPath $source -Recurse -Force)
if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
    throw 'Linked files or directories are not supported.'
}
if ((Get-Item -LiteralPath $source).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'SourceDirectory must not be a link.'
}
$fbx = @($items | Where-Object { !$_.PSIsContainer -and $_.Extension -ieq '.fbx' })
if ($fbx.Count -ne 1) { throw 'Expected exactly one FBX. Choose a source directory containing one model.' }
$textures = @($items | Where-Object { !$_.PSIsContainer -and $_.Extension -ieq '.png' } | Sort-Object FullName)
$assets = @([pscustomobject]@{ Source = $fbx[0].FullName; Relative = 'model.fbx'; Kind = 'fbx' })
foreach ($texture in $textures) {
    $relative = $texture.FullName.Substring($sourcePrefix.Length)
    $assets += [pscustomobject]@{ Source = $texture.FullName; Relative = $relative; Kind = 'texture' }
}

# Existing content must match exactly; do not overwrite an edited model or manifest.
$entries = @()
foreach ($asset in $assets) {
    $destination = [IO.Path]::GetFullPath((Join-Path $target $asset.Relative))
    if (!$destination.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Asset path escapes the model directory: $($asset.Relative)"
    }
    $hash = (Get-FileHash -LiteralPath $asset.Source -Algorithm SHA256).Hash.ToLowerInvariant()
    if (Test-Path -LiteralPath $destination) {
        if (!(Test-Path -LiteralPath $destination -PathType Leaf) -or
            (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) {
            throw "Destination differs; left untouched: $destination"
        }
    }
    $entries += [ordered]@{
        path = $asset.Relative.Replace('\', '/')
        kind = $asset.Kind
        bytes = (Get-Item -LiteralPath $asset.Source).Length
        sha256 = $hash
        sourcePath = $asset.Source
    }
}
$manifest = [ordered]@{
    schemaVersion = 1
    modelId = $ModelId
    status = 'assets-only; P0 blocked; runtime importer not implemented'
    sourceDirectory = $source
    files = $entries
}
$manifestText = ($manifest | ConvertTo-Json -Depth 6) + [Environment]::NewLine
$manifestPath = Join-Path $target 'asset-manifest.json'
if ((Test-Path -LiteralPath $manifestPath) -and
    [IO.File]::ReadAllText($manifestPath) -cne $manifestText) {
    throw "Manifest differs; left untouched: $manifestPath"
}

foreach ($asset in $assets) {
    $destination = Join-Path $target $asset.Relative
    if (!(Test-Path -LiteralPath $destination)) {
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        [IO.File]::Copy($asset.Source, $destination, $false)
    }
    $actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    $expected = ($entries | Where-Object { $_.sourcePath -ceq $asset.Source }).sha256
    if ($actual -cne $expected) { throw "Copy verification failed: $destination" }
}
if (!(Test-Path -LiteralPath $manifestPath)) {
    [IO.File]::WriteAllText($manifestPath, $manifestText, [Text.UTF8Encoding]::new($false))
}
# P2 is deliberately not started: no fabricated bone/clip mapping, model.json,
# FBX rewrite, runtime toggle, texture registration, or render hook is installed.
Write-Host "Verified $($assets.Count) assets: $target"
Write-Host 'Assets are staged only. P0/P1 and the runtime FBX importer are not implemented.'
