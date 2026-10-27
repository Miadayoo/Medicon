[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('Auto', 'MSVC', 'LLVM')][string]$Toolchain = 'Auto',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$localCmake = Join-Path $projectRoot '.tools/python/cmake/data/bin/cmake.exe'
$localCompiler = Join-Path $projectRoot '.tools/llvm-mingw-20260616-ucrt-x86_64/bin/clang++.exe'
$localNinja = Join-Path $projectRoot '.tools/python/bin/ninja.exe'
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakeExe = $cmakeCommand.Source }
elseif (Test-Path -LiteralPath $localCmake) { $cmakeExe = $localCmake }
else { throw 'CMake 3.25+ is required. Install Visual Studio C++ tools with CMake, then retry.' }
$ctestExe = Join-Path (Split-Path $cmakeExe) 'ctest.exe'

if ($Toolchain -eq 'Auto') {
    if (Test-Path -LiteralPath $localCompiler) { $Toolchain = 'LLVM' }
    else { $Toolchain = 'MSVC' }
}

Push-Location $projectRoot
try {
    if ($Toolchain -eq 'LLVM') {
        if (!(Test-Path -LiteralPath $localCompiler) -or !(Test-Path -LiteralPath $localNinja)) {
            throw 'Local LLVM MinGW and Ninja were not found under .tools. Use -Toolchain MSVC with Visual Studio 2022 C++ tools, or provision the local toolchain.'
        }
        $buildDirectory = Join-Path $projectRoot 'out/build/local'
        $cCompiler = Join-Path (Split-Path $localCompiler) 'clang.exe'
        & $cmakeExe -S $projectRoot -B $buildDirectory -G Ninja `
            "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_C_COMPILER=$cCompiler" `
            "-DCMAKE_CXX_COMPILER=$localCompiler" "-DCMAKE_MAKE_PROGRAM=$localNinja" `
            '-DBUILD_TESTING=ON' '-DMEDICON_BUILD_WINDOWS=ON' `
            '-DCMAKE_EXE_LINKER_FLAGS=-static' '-DCMAKE_SHARED_LINKER_FLAGS=-static'
    } else {
        $buildDirectory = Join-Path $projectRoot 'out/build/windows'
        & $cmakeExe --preset windows
    }
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    & $cmakeExe --build $buildDirectory --config $Configuration --parallel 6
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    if (!$SkipTests) {
        & $ctestExe --test-dir $buildDirectory -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
    Write-Host "Build complete: $buildDirectory/bin"
} finally {
    Pop-Location
}
