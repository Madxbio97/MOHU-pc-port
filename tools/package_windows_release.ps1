param(
    [string]$Version = "0.1.2",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build\windows-psycross\$Configuration"
$distDir = Join-Path $repoRoot "dist"
$packageName = "Medal-of-Honor-Underground-PC-$Version-win64"
$packageDir = Join-Path $distDir $packageName
$archivePath = Join-Path $distDir "$packageName.zip"
$archiveHashPath = "$archivePath.sha256"

foreach ($path in @($packageDir, $archivePath, $archiveHashPath)) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to overwrite an existing release artifact: $path"
    }
}

$runtimeFiles = @(
    "medal_of_honor_underground.exe",
    "avcodec-62.dll",
    "avformat-62.dll",
    "avutil-60.dll",
    "fmt.dll",
    "OpenAL32.dll",
    "SDL2.dll",
    "swresample-6.dll",
    "swscale-9.dll"
)

foreach ($file in $runtimeFiles) {
    $source = Join-Path $buildDir $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required runtime file is missing: $source"
    }
}

$skyboxSource = Join-Path $buildDir "assets\skyboxes"
$skyboxFiles = @(
    "midnight_paris.tga",
    "french_village_night.tga",
    "desert_morning.tga",
    "desert_storm.tga",
    "sandstorm.tga",
    "greek_coast_night.tga",
    "wewelsburg_fog.tga",
    "wewelsburg_night.tga",
    "monte_cassino_night.tga",
    "french_mountains_night.tga",
    "french_mountains_morning.tga",
    "paris_outskirts_night.tga"
)
foreach ($file in $skyboxFiles) {
    $source = Join-Path $skyboxSource $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required skybox is missing: $source"
    }
}
$localeSource = Join-Path $repoRoot "assets\locales\ru-vit"
if (-not (Test-Path -LiteralPath $localeSource -PathType Container)) {
    throw "Required Russian localization pack is missing: $localeSource"
}

$vcRedistRoots = [Collections.Generic.List[string]]::new()
if ($env:VCToolsRedistDir) {
    $vcRedistRoots.Add($env:VCToolsRedistDir)
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
    $installations = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Redist.14.Latest -property installationPath
    foreach ($installation in $installations) {
        if ($installation) {
            $vcRedistRoots.Add((Join-Path $installation "VC\Redist\MSVC"))
        }
    }
}
$vcRuntimeDir = $vcRedistRoots |
    Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
    ForEach-Object {
        Get-ChildItem -LiteralPath $_ -Recurse -Filter "vcruntime140.dll" -File -ErrorAction SilentlyContinue
    } |
    Where-Object { $_.FullName -match "\\x64\\Microsoft\.VC\d+\.CRT\\vcruntime140\.dll$" -and $_.FullName -notmatch "\\onecore\\" } |
    Sort-Object FullName -Descending -Unique |
    Select-Object -First 1 -ExpandProperty DirectoryName
if (-not $vcRuntimeDir) {
    throw "Microsoft Visual C++ x64 runtime was not found."
}
$vcFiles = @(
    "msvcp140.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)

New-Item -ItemType Directory -Path $packageDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageDir "assets\skyboxes") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageDir "licenses") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageDir "locales") | Out-Null

foreach ($file in $runtimeFiles) {
    Copy-Item -LiteralPath (Join-Path $buildDir $file) -Destination $packageDir
}
foreach ($file in $skyboxFiles) {
    Copy-Item -LiteralPath (Join-Path $skyboxSource $file) -Destination (Join-Path $packageDir "assets\skyboxes")
}
Copy-Item -LiteralPath $localeSource -Destination (Join-Path $packageDir "locales") -Recurse

foreach ($file in $vcFiles) {
    $source = Join-Path $vcRuntimeDir $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required Visual C++ runtime file is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination $packageDir
}

Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $packageDir "LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $repoRoot "external\PsyCross\LICENSE") -Destination (Join-Path $packageDir "licenses\PsyCross.txt")

$vcpkgShare = Join-Path $repoRoot "build\windows-psycross\vcpkg_installed\x64-windows\share"
$licenseSources = @{
    "FFmpeg.txt" = Join-Path $vcpkgShare "ffmpeg\copyright"
    "fmt.txt" = Join-Path $vcpkgShare "fmt\copyright"
    "OpenAL-Soft.txt" = Join-Path $vcpkgShare "openal-soft\copyright"
    "SMAA.txt" = Join-Path $repoRoot "external\PsyCross\src\render\smaa\LICENSE.txt"
    "SDL2.txt" = Join-Path $vcpkgShare "sdl2\copyright"
}
foreach ($entry in $licenseSources.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) {
        throw "Required license file is missing: $($entry.Value)"
    }
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $packageDir "licenses\$($entry.Key)")
}

$buildDate = Get-Date -Format "yyyy-MM-dd"
$readme = @"
MEDAL OF HONOR: UNDERGROUND PC — $Version
Windows x64, $buildDate

This unofficial package does not contain game data. A legally obtained USA
SLUS-01270 BIN/CUE image is required. Other versions are not supported.

1. Extract the ZIP to a separate directory.
2. Run medal_of_honor_underground.exe.
3. Select the CUE file and configure graphics and controls.

Keep every BIN file beside its CUE file. Settings and saves are stored in:
%LOCALAPPDATA%\MedalOfHonorUndergroundPC

Requirements: Windows 10/11 x64 and an OpenGL 3.x capable GPU/driver.

This project is not affiliated with or endorsed by Electronic Arts or Sony.
"@
$notices = @"
This package includes or dynamically links third-party software: FFmpeg, SDL2,
OpenAL Soft, fmt, PsyCross, SMAA and the Microsoft Visual C++ Runtime. Supplied
license texts are in the licenses directory.
"@
$utf8 = New-Object System.Text.UTF8Encoding($true)
[IO.File]::WriteAllText((Join-Path $packageDir "README_FIRST.txt"), $readme, $utf8)
[IO.File]::WriteAllText((Join-Path $packageDir "THIRD_PARTY_NOTICES.txt"), $notices, $utf8)

$forbidden = Get-ChildItem -LiteralPath $packageDir -Recurse -File | Where-Object {
    $_.Name -match "(?i)(cheats|save|\.sav(?:\.bak)?$|\.cue$|\.bin$|\.iso$|\.img$|\.chd$|\.cmd$|\.log$|\.dmp$|\.obj$|\.pdb$|\.ilk$|\.lib$|\.exp$|test|probe|diagnostic|trace)"
}
if ($forbidden) {
    throw "Forbidden files found in release: $($forbidden.FullName -join ', ')"
}

$hashLines = Get-ChildItem -LiteralPath $packageDir -Recurse -File |
    Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($packageDir.Length + 1).Replace("\", "/")
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
        "$hash  $relative"
    }
[IO.File]::WriteAllLines((Join-Path $packageDir "SHA256SUMS.txt"), $hashLines, $utf8)

Compress-Archive -LiteralPath $packageDir -DestinationPath $archivePath -CompressionLevel Optimal
$archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($archiveHashPath, "$archiveHash  $packageName.zip`r`n", $utf8)

Write-Output $packageDir
Write-Output $archivePath
Write-Output $archiveHashPath
