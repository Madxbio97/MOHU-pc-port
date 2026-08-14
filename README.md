# Medal of Honor: Underground PC

Unofficial Windows port of the USA/NTSC-U PlayStation release of *Medal of
Honor: Underground* (`SLUS-01270`). The original R3000A game code remains the
gameplay authority; the host supplies rendering, audio, input, storage and
windowing through PsyCross.

The game is playable, but performance and PGXP rendering are still being tuned.
Gameplay validation is manual; deterministic CPU, GPU, SPU and platform tests
cover the reusable runtime.

No game data is included. A legally obtained BIN/CUE image is required. This
project is not affiliated with Electronic Arts, Sony or the original developers.

## Supported image

| Field | Value |
| --- | --- |
| Region | USA / NTSC-U |
| Serial | `SLUS-01270` |
| Volume ID | `MOHU_NTSC` |
| Format | MODE2/2352 BIN or matching CUE |
| PS-X EXE SHA-256 | `5c4566b7264a29554abb9d868f23cc40e99ebf3b58b9776dfe2f4ae465bbd641` |

Other regions and revisions are rejected deliberately.

## Build

Requirements: Windows 10/11 x64, Visual Studio C++, CMake 3.24+ and vcpkg.

```powershell
$env:VCPKG_ROOT = 'D:/VS2026/VC/vcpkg'
cmake --preset windows-psycross
cmake --build --preset windows-psycross-app-release
```

Executable:

```text
build/windows-psycross/Release/medal_of_honor_underground.exe
```

Full Release validation:

```powershell
cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release
```

## Run

Start the executable and select the supported BIN/CUE in the launcher, or use:

```powershell
./build/windows-psycross/Release/medal_of_honor_underground.exe `
  --no-launcher 'D:/Games/Medal of Honor - Underground (USA).cue'
```

See [documentation](docs/README.md) for controls, build details and testing.

## Source layout

| Path | Purpose |
| --- | --- |
| `apps/medal_of_honor_underground/` | Launcher and application entry point |
| `include/mohu/`, `src/mohu/` | MOHU guest runtime and GPU command capture |
| `include/sf/`, `src/` | Reused portable PS1 and platform foundation |
| `external/PsyCross/` | Vendored renderer, audio, input and window backend |
| `tests/` | Deterministic validation |
| `apps/mohu_*_probe.cpp` | Optional legal-ROM validation tools |
