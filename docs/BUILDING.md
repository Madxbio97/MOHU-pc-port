# Build and validation

## Requirements

- Windows 10/11 x64.
- Visual Studio C++ with the Windows SDK.
- CMake 3.24 or newer.
- vcpkg with SDL2, OpenAL Soft and FFmpeg dependencies.

PsyCross is vendored under `external/PsyCross`.

## Playable Release

```powershell
$env:VCPKG_ROOT = 'D:/VS2026/VC/vcpkg'
cmake --preset windows-psycross
cmake --build --preset windows-psycross-app-release
```

Output:

```text
build/windows-psycross/Release/medal_of_honor_underground.exe
```

## Full Release and tests

```powershell
cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release
```

Portable targets can be built without PsyCross:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release
ctest --preset windows-release
```

## Legal-ROM tests

ROM tests are opt-in and never download or copy game data:

```powershell
cmake --preset windows-psycross `
  -DMOHU_SUPPORTED_ROM_CUE='D:/Games/Medal of Honor - Underground (USA).cue'
cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release -L rom
```

## Validation policy

- Run `git diff --check` before committing.
- Build the playable Release after runtime or platform changes.
- Run focused tests during iteration and the full CTest preset before release.
- Leave long gameplay, visual comparison and controller testing to a manual pass.
