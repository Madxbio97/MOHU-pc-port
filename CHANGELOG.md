# Changelog

## 0.1.1 - 2026-08-19

### Runtime

- Made LEVEL2P split-screen presentation follow the single-player output
  resolution and adaptive-aspect pipeline.
- Removed resolution-dependent multiplayer world rejection and admitted every
  coarse BSP sector while preserving final triangle/object clipping.
- Disabled geometry and performance diagnostics by default; they remain
  available through their explicit environment switches.

### Release

- Updated the clean Windows x64 package metadata.

## 0.1.0 - 2026-08-18

### Runtime

- Preserved exact signed view-space depth for close-camera perspective
  correction.
- Removed unused native-scene and native-world prototype render paths.
- Removed production frame diagnostics, VRAM capture polling and inactive CPU
  store tracing from hot loops.

### Repository

- Imported the current MOHU PC port as the repository baseline.
- Removed Syphon Filter-only probes, release notes and historical research.
- Replaced stale project documentation with MOHU-specific build and runtime
  guidance.
