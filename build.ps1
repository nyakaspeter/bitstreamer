# Configures and builds bitstreamer with the Visual Studio toolchain and vcpkg.
#   .\build.ps1                 # release build with GUI
#   .\build.ps1 -Preset release-nogui
#   .\build.ps1 -Test           # also run the automated tests
#   .\build.ps1 -Package        # also create the ZIP release package
param(
    [string]$Preset = "release",
    [switch]$Test,
    [switch]$Package
)
# Native tools write progress to stderr; failures are detected through exit codes.
$ErrorActionPreference = "Continue"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio (with the C++ workload) is required." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the MSVC x64 toolset was found." }

# Import the MSVC x64 environment into this PowerShell session.
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

# Use VCPKG_ROOT if it points at a real vcpkg, otherwise the copy bundled with Visual Studio.
if (-not $env:VCPKG_ROOT -or -not (Test-Path (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"))) {
    $env:VCPKG_ROOT = Join-Path $vs "VC\vcpkg"
}
if (-not (Test-Path (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"))) {
    throw "vcpkg not found. Install the 'vcpkg package manager' Visual Studio component or set VCPKG_ROOT."
}

Push-Location $PSScriptRoot
try {
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw "configure failed" }
    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "build failed" }
    if ($Test) {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw "tests failed" }
    }
    if ($Package) {
        Push-Location "build\$Preset"
        try { cpack -G ZIP; if ($LASTEXITCODE) { throw "packaging failed" } } finally { Pop-Location }
    }
} finally {
    Pop-Location
}
