# Builds bitstreamer with the Visual Studio toolchain: the core library and
# tests with CMake and vcpkg, then the WinUI 3 app with MSBuild and NuGet.
#   .\build.ps1                 # build everything
#   .\build.ps1 -Test           # also run the automated tests
#   .\build.ps1 -Package        # also create the ZIP release package
param(
    [switch]$Test,
    [switch]$Package
)
# Native tools write progress to stderr; failures are detected through exit codes.
$ErrorActionPreference = "Continue"
$Preset = "release"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio (with the C++ workload) is required." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the MSVC x64 toolset was found." }
$msbuild = Join-Path $vs "MSBuild\Current\Bin\amd64\MSBuild.exe"

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
    # Core library and tests.
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw "configure failed" }
    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "build failed" }
    if ($Test) {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw "tests failed" }
    }

    # The app. The Windows App SDK comes from NuGet.
    $project = "src\app\bitstreamer.vcxproj"
    $common = @("-p:Configuration=Release", "-p:Platform=x64", "-p:CorePreset=$Preset", "-nologo", "-v:m", "-m", "-nodeReuse:false")
    & $msbuild $project -t:restore -p:RestorePackagesConfig=true @common
    if ($LASTEXITCODE) { throw "package restore failed" }
    & $msbuild $project @common
    if ($LASTEXITCODE) { throw "app build failed" }

    if ($Package) {
        $version = (Get-Content vcpkg.json -Raw | ConvertFrom-Json).'version-string'
        $name = "bitstreamer-$version-win64"
        $stage = "build\package\$name"
        if (Test-Path "build\package") { Remove-Item "build\package" -Recurse -Force }
        New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null
        Copy-Item "build\app\bin\*" $stage -Recurse -Exclude *.pdb, *.lib, *.exp
        Copy-Item README.md, LICENSE, THIRD_PARTY_NOTICES.md $stage
        Copy-Item docs $stage -Recurse
        # License texts of the bundled third-party software.
        Copy-Item "build\$Preset\vcpkg_installed\x64-windows\share\ffmpeg\copyright" "$stage\licenses\ffmpeg.txt"
        $sdk = Get-ChildItem "build\packages" -Directory -Filter "Microsoft.WindowsAppSDK.*" | Select-Object -First 1
        Copy-Item (Join-Path $sdk.FullName "license.txt") "$stage\licenses\windows-app-sdk.txt"
        Copy-Item (Join-Path $sdk.FullName "NOTICE.txt") "$stage\licenses\windows-app-sdk-notice.txt"
        $zip = "build\$name.zip"
        if (Test-Path $zip) { Remove-Item $zip -Force }
        Compress-Archive -Path $stage -DestinationPath $zip
        Write-Host "Package: $zip"
    }
} finally {
    Pop-Location
}
