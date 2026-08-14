# Troubleshooting

## The image is rejected

Only the USA/NTSC-U `SLUS-01270` release is supported. Confirm that the CUE uses
the correct BIN filenames and that every track is present. The expected PS-X
EXE SHA-256 is:

```text
5c4566b7264a29554abb9d868f23cc40e99ebf3b58b9776dfe2f4ae465bbd641
```

## The program does not start

Build or extract the complete application directory. Keep the PsyCross runtime
DLLs beside the executable. Update the GPU driver and install the current Visual
C++ runtime when using a packaged build.

## Settings do not persist

Check write access to:

```text
%LOCALAPPDATA%\MedalOfHonorUndergroundPC
```

Delete only `launcher.ini` to reset launcher settings. Do not delete the `Saves`
directory unless the virtual memory card is backed up.

## Performance is unexpectedly low

- Use a Release build.
- Disable VSync and select Unlimited while measuring throughput.
- Compare at the same resolution, filtering and antialiasing settings.
- Disable overlays and external capture software for the comparison run.
- Reproduce the same scene and camera path.

Long gameplay and visual checks are manual. Automated tests validate the CPU,
GPU command path, SPU and platform policies but do not replace a gameplay pass.
