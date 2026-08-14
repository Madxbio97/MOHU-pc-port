# User guide

## Requirements

- Windows 10 or 11, x64.
- A legally obtained USA/NTSC-U BIN/CUE image of *Medal of Honor: Underground*
  (`SLUS-01270`).

The launcher validates the PS-X executable and rejects unsupported revisions.

## Starting the game

1. Run `medal_of_honor_underground.exe`.
2. Select the BIN or CUE when prompted.
3. Choose resolution, filtering, antialiasing, aspect ratio, frame limit,
   controller backend and input bindings.
4. Start the game.

The selected image and launcher settings are stored in:

```text
%LOCALAPPDATA%\MedalOfHonorUndergroundPC\launcher.ini
```

The virtual PlayStation memory card is stored in:

```text
%LOCALAPPDATA%\MedalOfHonorUndergroundPC\Saves\SLUS-01270.mcr
```

Back up the `.mcr` file before replacing builds or moving saves between
machines.

## Command line

Useful options:

```text
--no-launcher
--fullscreen | --windowed
--resolution=WIDTHxHEIGHT
--filter=nearest|bilinear|trilinear|anisotropic
--aa=off|smaa|fxaa
--aspect-adaptive | --aspect-4-3
--vsync | --no-vsync
--fps-limit=0|20..1000
--controller-backend=auto|xinput|dinput|rawinput
--mouse-sensitivity=25..400
--language=en|ru
```

Use a positional BIN/CUE path with `--no-launcher` for direct startup.

## Performance

For throughput testing, use a Release build, disable VSync and set the frame
limit to Unlimited. Keep resolution, filtering and antialiasing identical when
comparing revisions.
