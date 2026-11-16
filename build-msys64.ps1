<#
.SYNOPSIS
Build the Windows x86 client with the MSYS2 clang toolchain.
.DESCRIPTION
Uses the ucrt64 clang from an MSYS2 installation to cross-compile for
i686-w64-windows-gnu, taking headers, libraries and runtime from the
installed mingw32 GCC toolchain.
Requires the following MSYS2 packages:
  mingw-w64-ucrt-x86_64-clang, mingw-w64-ucrt-x86_64-lld,
  mingw-w64-i686-gcc, mingw-w64-ucrt-x86_64-make,
  mingw-w64-ucrt-x86_64-ninja, mingw-w64-ucrt-x86_64-cmake and Python 3.9+.
Dependencies are built from the vendored sources; no downloads are performed.
.EXAMPLE
.\build-msys64.ps1
.EXAMPLE
.\build-msys64.ps1 -Msys2Root C:\msys64 -Jobs 8
.EXAMPLE
.\build-msys64.ps1 -CMake C:\Tools\cmake.exe -Ninja C:\Tools\ninja.exe
#>
[CmdletBinding()]
param(
    [string]$Msys2Root = 'C:\msys64',
    [string]$CMake = 'cmake',
    [string]$Ninja,
    [string]$BuildDirectory,
    [ValidateRange(1, 256)]
    [int]$Jobs = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot 'build\msys64-clang-x86'
}
if (-not (Test-Path -LiteralPath $Msys2Root -PathType Container)) {
    throw "MSYS2 root '$Msys2Root' does not exist. Pass -Msys2Root explicitly."
}
$msys2Path = (Resolve-Path -LiteralPath $Msys2Root).Path
$ucrtBin = Join-Path $msys2Path 'ucrt64\bin'
$mingwBin = Join-Path $msys2Path 'mingw32\bin'

$requiredTools = @(
    (Join-Path $ucrtBin 'clang.exe'),
    (Join-Path $ucrtBin 'clang++.exe'),
    (Join-Path $ucrtBin 'windres.exe'),
    (Join-Path $mingwBin 'ar.exe'),
    (Join-Path $mingwBin 'ranlib.exe')
)
$missingTools = @($requiredTools | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
if ($missingTools.Count -ne 0) {
    throw ("Missing MSYS2 clang tools:`n  " + ($missingTools -join "`n  ") +
        "`nInstall them with: pacman -S --needed mingw-w64-ucrt-x86_64-clang " +
        "mingw-w64-ucrt-x86_64-lld mingw-w64-i686-gcc")
}
foreach ($sysrootPart in @('include', 'lib')) {
    if (-not (Test-Path -LiteralPath (Join-Path $msys2Path "mingw32\$sysrootPart") -PathType Container)) {
        throw ("The mingw32 i686 sysroot is incomplete (missing mingw32\$sysrootPart).`n" +
            "Install it with: pacman -S --needed mingw-w64-i686-gcc")
    }
}

$env:PATH = "$ucrtBin;$mingwBin;$env:PATH"

$makePath = Join-Path $ucrtBin 'mingw32-make.exe'
if (-not (Test-Path -LiteralPath $makePath -PathType Leaf)) {
    throw ("mingw32-make.exe was not found in $ucrtBin.`n" +
        "Install it with: pacman -S --needed mingw-w64-ucrt-x86_64-make")
}

if ([string]::IsNullOrWhiteSpace($Ninja)) {
    $ninjaCandidates = @(
        (Join-Path $ucrtBin 'ninja.exe'),
        (Join-Path $mingwBin 'ninja.exe')
    )
    $ninjaPath = $ninjaCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $ninjaPath) {
        $ninjaPath = (Get-Command 'ninja' -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1).Source
    }
} else {
    $ninjaPath = (Get-Command $Ninja -CommandType Application -ErrorAction Stop |
        Select-Object -First 1).Source
}
if ([string]::IsNullOrWhiteSpace($ninjaPath)) {
    throw ("Ninja was not found.`n" +
        "Install it with: pacman -S --needed mingw-w64-ucrt-x86_64-ninja")
}

$cmakePath = (Get-Command $CMake -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source
$buildPath = [System.IO.Path]::GetFullPath($BuildDirectory)
if ($buildPath.TrimEnd('\', '/') -eq $PSScriptRoot.TrimEnd('\', '/')) {
    throw 'Use a separate build directory; generated files must not be written into the source root.'
}

& $cmakePath -S $PSScriptRoot -B $buildPath -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$PSScriptRoot/cmake/msys2-clang-x86.cmake" `
    "-DMSYS2_ROOT=$msys2Path" `
    "-DSKEET_LUAJIT_MAKE=$makePath" `
    "-DCMAKE_MAKE_PROGRAM=$ninjaPath" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed (exit $LASTEXITCODE)."
}

& $cmakePath --build $buildPath --target skeet_client skeet_core_linkcheck --parallel $Jobs
if ($LASTEXITCODE -ne 0) {
    throw "Client build failed (exit $LASTEXITCODE)."
}
Write-Output (Join-Path $buildPath 'bin\skeet_client.dll')
