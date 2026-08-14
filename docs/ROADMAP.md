# Roadmap

## Performance

- Identify remaining CPU, renderer and SPU hot-loop regressions.
- Maintain stable frame pacing from 60 through 240 FPS.
- Avoid diagnostics, allocations and zero-work submissions in production loops.

## Rendering

- Remove remaining polygon micro-seams without changing packet order.
- Preserve perspective-correct texturing at extreme close-camera depth.
- Keep fallback decisions primitive-atomic and provenance-based.

## Validation

- Expand deterministic tests for every fixed regression.
- Maintain repeatable saves and camera paths for manual comparison.
- Run long gameplay, audio and controller checks before release.
