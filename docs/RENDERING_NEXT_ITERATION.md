# Rendering pipeline: next iteration contract

Updated 2026-08-12.

## Production invariants

- Exact camera, model-root and vertex transforms are captured before Q12-to-Q8
  truncation and are generation stamped.
- A precise projection is accepted only through transported identity plus the
  original packed coordinate witness. Packed-SXY lookup is not vertex identity.
- Perspective depth is the hardware-visible, positive projective depth
  max(H / 2, SZ). Unclamped view coordinates stabilize screen XY but never
  supply an unbounded or signed raster W.
- Perspective W is primitive atomic. If one vertex lacks a coherent W, the
  complete primitive falls back; precise XY may remain only when its provenance
  is independently valid.
- The raw guest renderer uses reversed depth only for coherent 3D primitives.
  UI and legacy packets retain PS1 ordering; transparent geometry tests depth
  without writing it.
- Widescreen expands the verified guest world-frustum intervals and applies one
  presentation scale. It does not mutate GTE H, OFX or UI geometry.
- GPU DMA is completed before the frame-local projection catalog is recycled.

## Quarantined diagnostics

F9 quad recovery and F10 coherence recovery are intentionally disabled by
default. They reconstruct or snap data without complete guest provenance and
must never be enabled as a shipping fix. Their code remains only for controlled
A/B diagnosis and is excluded from the normal submit fast path.

The full diagnostic map is logged at startup:

| Key | Feature |
| --- | --- |
| F1 | Perspective-correct textures |
| F2 | Precise screen XY |
| F3 | Exact transform |
| F4 | Exact capture |
| F5 | Compact catalog |
| F6 | Identity sidecar |
| F7 | Preserve projection precision |
| F8 | Positive projective-depth clamp |
| F9 | Diagnostic quad recovery |
| F10 | Diagnostic coherence recovery |
| F11 | Primitive-atomic fallback |
| F12 | Geometry master switch |

## Measurement contract

For the next first-level run, preserve the log from one uninterrupted minute
and record the same static wall/floor view for at least ten seconds. Relevant
signals are [RuntimeDiag], [GuestGpuDiag], [GeometryToggle], precise versus
missing primitive counts, submit/render maxima, guest Hz and audio recovery.

Do not tune pixel tolerances. A remaining seam must be traced to a concrete
producer PC, packet source address, projection identity generation and
primitive missing-mask.

## Ordered next step

1. Capture per-source primitive-class transitions for the static TSP GT3
   producer (0x80010CF0, return 0x8009A51C).
2. Separate original exact vertices from retail screen-clipped intersections.
3. Replace the validated static-world packet interval with cached TSP geometry
   only after camera generation, source range and material/VRAM ownership are
   explicit.
4. Clip in homogeneous camera space and retain primitive-atomic perspective W.
5. Benchmark guest CPU, submit and render independently before enabling the new
   path by default.

The retired post-frame native projection prototype is preserved under
docs/research/retired_native_world and is not linked into production.