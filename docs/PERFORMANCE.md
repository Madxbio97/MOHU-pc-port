# Performance validation

## Target

The supported operating range is 60-240 presentation FPS. A 240 FPS run has a
4.17 ms frame budget; the original guest simulation cadence remains unchanged.

## Setup

- Use a Release build.
- Disable VSync and select Unlimited for throughput measurements.
- Keep resolution, filtering, antialiasing and aspect settings identical.
- Disable third-party overlays and capture tools.
- Record the commit, hardware, driver and settings.

## Protocol

1. Start a new process for each run.
2. Load the same save/checkpoint and camera position.
3. Warm the scene for 30-60 seconds.
4. Capture at least 60 seconds.
5. Repeat three times and compare the median run.

Use at least three workloads: an open scene, dense combat and a close-camera
geometry pass. Include a slow 360-degree sweep to expose streaming and shader
state changes.

## Acceptance

- No unexplained average or 95th-percentile regression above 5%.
- Stable pacing at the selected 60, 120 or 240 FPS cap.
- No post-warm-up hitch above 16.67 ms at the 60 FPS target.
- Rendering, audio and gameplay remain correct.
- `ctest --preset windows-psycross-release` passes.

Automated tests do not measure gameplay frame time. Final performance and visual
approval is a manual gameplay task.
