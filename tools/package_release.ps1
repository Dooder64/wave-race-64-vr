# Stage a built tree into a release folder and zip it.
#
#   powershell -ExecutionPolicy Bypass -File tools/package_release.ps1 -BuildDir build-fe -Version 0.1.0
#
# What goes in: the executable, the three DLLs it cannot start without (SDL2,
# dxcompiler, dxil), "Play in VR.bat" (the executable with WR64_VR=1), assets/
# (menus, replacement music, HD textures), this project's LICENSE, the
# third-party notices, the README, and licenses/ with each bundled component's
# license text. The debug symbols go into a
# second zip, which is only needed to investigate crash reports. Nothing here
# is a ROM, and the script refuses to
# continue if it finds one where it is staging.
#
# What the executable is. It contains the game's code, statically recompiled
# from the cartridge into C and compiled into the binary; that is what a
# recompiled port is. Original cartridge data is imported from the player's
# own dump at run time. Reviewed replacement music and textures are distributed
# separately in assets/ with their credits; see the README's Licensing section.

param(
    [string]$BuildDir = "build-fe",
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Version = "0.1.0",
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$exe = Join-Path $BuildDir "WaveRace64Recomp.exe"
if (-not (Test-Path $exe)) {
    throw "no executable at $exe -- build first (see docs/BUILDING.md)"
}

# Python 3.10+ is already required by the source-build toolchain. Validate the
# built assets against this checkout before creating or replacing a release.
& python "tools/bundled_assets.py" --assets (Join-Path $BuildDir "assets") --reference "assets"
if ($LASTEXITCODE -ne 0) { throw "built music/texture assets are missing, corrupt, or stale; rebuild first" }

$name = "WaveRace64Recomp-$Version-windows-x64"
$outFull = [System.IO.Path]::GetFullPath($(if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }))
$stage = [System.IO.Path]::GetFullPath((Join-Path $outFull $name))
if (-not $stage.StartsWith($outFull.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar,
                         [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "release staging path is outside the output directory: $stage"
}
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

$files = @(
    "$BuildDir\WaveRace64Recomp.exe",
    "$BuildDir\SDL2.dll",
    "$BuildDir\dxcompiler.dll",
    "$BuildDir\dxil.dll",
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "README.md",
    "tools\release\Play in VR.bat"
)
foreach ($f in $files) {
    if (-not (Test-Path $f)) { throw "missing $f" }
    Copy-Item $f $stage
}
Copy-Item -LiteralPath 'docs/WINDOWS_RELEASE.md' -Destination (Join-Path $stage 'INSTALLATION.md')

# The license texts of what the executable contains (THIRD_PARTY_NOTICES.md),
# which go with the binary: licenses\<component>.txt.
$licenses = Join-Path $stage "licenses"
New-Item -ItemType Directory -Force $licenses | Out-Null
$texts = [ordered]@{
    "N64ModernRuntime (GPL-3.0)"   = "lib/N64ModernRuntime/COPYING"
    "RecompFrontend (GPL-3.0)"     = "lib/RecompFrontend/COPYING"
    "N64Recomp"                    = "lib/N64ModernRuntime/N64Recomp/LICENSE"
    "RT64"                         = "lib/RT64/LICENSE"
    "RmlUi"                        = "lib/RecompFrontend/recompui/lib/RmlUi/LICENSE.txt"
    "lunasvg"                      = "lib/RecompFrontend/recompui/lib/lunasvg/LICENSE"
    "GamepadMotionHelpers"         = "lib/RecompFrontend/lib/GamepadMotionHelpers/LICENSE"
    "Lato and Noto Emoji fonts"    = "lib/RecompFrontend/recompui/lib/RmlUi/Samples/assets/LICENSE.txt"
    "PromptFont"                   = "assets/promptfont/LICENSE.txt"
    "SDL2"                         = "lib/RT64/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/COPYING.txt"
    "OpenXR loader (MIT option)"   = "lib/OpenXR-SDK/LICENSES/MIT.txt"
    "jsoncpp"                      = "lib/OpenXR-SDK/src/external/jsoncpp/LICENSE"
}
foreach ($dir in @(Get-ChildItem -Directory "lib/RT64/src/contrib", "lib/N64ModernRuntime/thirdparty")) {
    $text = Get-ChildItem -File $dir.FullName | Where-Object { $_.Name -match '^(licen[cs]e|copying)' } | Select-Object -First 1
    if ($text -and -not $texts.Contains($dir.Name)) { $texts[$dir.Name] = $text.FullName }
}
foreach ($name in $texts.Keys) {
    if (-not (Test-Path -LiteralPath $texts[$name])) { throw "missing license text $($texts[$name])" }
    Copy-Item -LiteralPath $texts[$name] -Destination (Join-Path $licenses "$name.txt")
}
Copy-Item -Recurse "$BuildDir\assets" (Join-Path $stage "assets")
& python "tools/bundled_assets.py" --assets (Join-Path $stage "assets") --reference "assets"
if ($LASTEXITCODE -ne 0) { throw "staged music/texture assets failed validation" }

# Belt and braces: the staging folder must contain no dump and no save.
$forbidden = Get-ChildItem -Recurse $stage | Where-Object {
    $_.Extension -in ".z64", ".n64", ".v64", ".rom", ".bin", ".eep", ".sra", ".fla"
}
if ($forbidden) {
    $forbidden | ForEach-Object { Write-Error "refusing to package $($_.FullName)" }
    throw "game data found in the staging folder"
}

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip

$pdb = Join-Path $BuildDir "WaveRace64Recomp.pdb"
if (Test-Path $pdb) {
    $symbols = Join-Path $OutDir "$name-debug-symbols.zip"
    if (Test-Path $symbols) { Remove-Item $symbols }
    Compress-Archive -Path $pdb -DestinationPath $symbols
    Write-Host "symbols: $symbols"
}

Write-Host "package: $zip"
$stageFull = (Resolve-Path $stage).Path
Get-ChildItem -Recurse $stage | Where-Object { -not $_.PSIsContainer } |
    ForEach-Object { "{0,10:N0}  {1}" -f $_.Length, $_.FullName.Substring($stageFull.Length + 1) }
