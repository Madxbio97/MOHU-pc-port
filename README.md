# Medal of Honor: Underground PC

Work-in-progress Windows port of the USA/NTSC-U PlayStation release of
Medal of Honor: Underground (SLUS-01270). The project reuses the proven
Syphon Filter PC and PsyCross foundations while keeping the original R3000A
game code in control.

The repository contains no game data. A legally obtained BIN or CUE image is
required. The project is unofficial and is not affiliated with Electronic Arts
or Sony Interactive Entertainment.

## Current status

- Native Windows launcher with remembered BIN/CUE selection and graphics,
  window, filtering, controller-backend and frame-limit settings.
- Exact validation of the USA executable:
  5c4566b7264a29554abb9d868f23cc40e99ebf3b58b9776dfe2f4ae465bbd641.
- Original executable booted in the R3000A interpreter.
- Shared BIOS HLE for bootstrap calls, critical sections and hardware IRQ
  dispatch/return.
- Register-level timers, DMA, CD-ROM and SPU foundation inherited from the
  reference port.
- Full raw BIN data track attached to the guest CD-ROM controller.
- Neutral digital-pad SIO protocol with a native active-low button bridge.
- PsyCross application frame loop now advances the original executable.

Rendering of the MOHU guest GPU stream, native audio presentation and complete
gameplay are still under active development. The current executable is a
runtime foundation, not a playable release.

## Supported image

| Field | Required value |
| --- | --- |
| Region | USA / NTSC-U |
| Serial | SLUS-01270 |
| Volume ID | MOHU_NTSC |
| Format | MODE2/2352 BIN or matching CUE |
| PS-X EXE SHA-256 | 5c4566b7264a29554abb9d868f23cc40e99ebf3b58b9776dfe2f4ae465bbd641 |

## Build

Prerequisites: Windows 10/11 x64, Visual Studio C++, CMake 3.24+, and vcpkg.
PsyCross is vendored in external/PsyCross.

    $env:VCPKG_ROOT = 'D:/VS2026/VC/vcpkg'
    cmake --preset windows-psycross
    cmake --build --preset windows-psycross-app-release

Output:

    build/windows-psycross/Release/medal_of_honor_underground.exe

Core build and focused validation:

    cmake --preset windows-msvc
    cmake --build build/windows-msvc --config Release
    ctest --test-dir build/windows-msvc -C Release --output-on-failure

Real-image checks:

    .\build\windows-psycross\Release\medal_of_honor_underground.exe --no-launcher --verify-only 'D:/Games/PS1/Medal of Honor - Underground (USA).bin'
    .\build\windows-msvc\Release\mohu_runtime_probe.exe 'D:/Games/PS1/Medal of Honor - Underground (USA).bin' 120

## Layout

| Path | Purpose |
| --- | --- |
| apps/medal_of_honor_underground/ | Launcher and Windows entry point |
| include/mohu/, src/mohu/ | MOHU guest runtime |
| include/sf/, src/ | Reused PS1 emulation and native platform foundation |
| external/PsyCross/ | Native renderer, input, audio and window backend |
| tests/ | Deterministic unit tests |
| apps/mohu_*_probe.cpp | Real-image bootstrap/runtime probes |
