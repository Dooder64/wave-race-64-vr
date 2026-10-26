# Build the standalone OpenXR test (tools/xr_test) with the same Visual Studio
# toolchain the game uses. The executable is build-xr-test/wr64_xr_test.exe.
param(
    [string]$BuildDir = 'build-xr-test',
    [string]$VisualStudioPath = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not (Test-Path 'lib/OpenXR-SDK/CMakeLists.txt')) {
        throw 'The OpenXR SDK submodule is missing. Run: git submodule update --init lib/OpenXR-SDK'
    }
    if (-not $VisualStudioPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools are required.' }
        $VisualStudioPath = @(& $vswhere -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
        if (-not $VisualStudioPath) { throw 'Install the Visual Studio Desktop development with C++ workload.' }
    }
    # Quoted PATH entries otherwise break the developer shell's batch setup.
    $env:PATH = ($env:PATH -split ';' | ForEach-Object { $_.Trim('"') }) -join ';'
    Import-Module (Join-Path $VisualStudioPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $VisualStudioPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    $clangDir = Join-Path $VisualStudioPath 'VC/Tools/Llvm/x64/bin'
    if (Test-Path (Join-Path $clangDir 'clang-cl.exe')) { $env:PATH = "$clangDir;$env:PATH" }

    & cmake -S tools/xr_test -B $BuildDir -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
    if ($LASTEXITCODE -ne 0) { throw 'OpenXR test CMake configuration failed.' }
    & cmake --build $BuildDir --target wr64_xr_test
    if ($LASTEXITCODE -ne 0) { throw 'OpenXR test build failed.' }
    Write-Host "Built: $(Join-Path $BuildDir 'wr64_xr_test.exe')"
} finally {
    Pop-Location
}
