# Architecture

The port is a hybrid runtime. The original MOHU R3000A program owns gameplay;
the Windows host owns platform services and presentation.

## Ownership

- Guest: gameplay state, scripts, actors, physics, GPU commands and SPU state.
- Host: physical input, windowing, rendering, audio output and persistent files.
- Bridges: PAD samples enter the guest; immutable GPU/SPU output leaves it.

The host must not synthesize or overwrite gameplay state.

## Main targets

| Target | Responsibility |
| --- | --- |
| `sf_core` | Errors, hashes and checked file I/O |
| `sf_disc` | BIN/CUE, ISO9660 and raw sectors |
| `sf_psx` | R3000A, GTE, DMA, timers, CD-ROM, SPU and XA |
| `sf_game` | Shared disc and runtime support inherited from the base port |
| `mohu_runtime` | MOHU boot, BIOS HLE, guest cadence and GPU capture |
| `sf_psycross_backend` | OpenGL, OpenAL, SDL input and frame presentation |
| `medal_of_honor_underground` | Shipping launcher and executable |

`external/PsyCross` is vendored third-party code with local renderer changes.

## Rendering

The guest GP0 stream is submitted in packet order. Projection sidecars carry
exact GTE provenance to PGXP; a primitive falls back atomically when exact
perspective depth cannot be proven. Close-camera signed view-space depth must
remain unclamped so perspective-correct interpolation does not collapse.

## Timing

Guest simulation advances on its recovered cadence. Presentation may run at a
higher refresh rate without advancing guest time. Audio output drains emulated
SPU PCM through a bounded host queue.
