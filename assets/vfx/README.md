# Atmospheric VFX atlas

`source/` contains the five generated high-resolution source cards used by
the optional depth-aware volumetric renderer:

1. fire;
2. explosion;
3. smoke;
4. fog/vapor;
5. lamp halo.

The images were generated specifically for this project with OpenAI ImageGen.
They contain isolated effects on black and no third-party game artwork.
`atmosphere_atlas.png` is data, not an ordinary colour image: every RGBA
channel stores a different depth slice. `atmosphere_atlas_preview.png` shows
the maximum density of those slices for visual review.

Regenerate the checked-in atlas and embedded runtime header with:

```powershell
python tools/generate_volumetric_atlas.py `
  assets/vfx/source `
  assets/vfx/atmosphere_atlas.png `
  assets/vfx/atmosphere_atlas_preview.png `
  src/platform/volumetric_atlas_texture.hpp
```

Cell layout is a 4x2 grid: fire, explosion, smoke and fog occupy the first
row; halo starts the second row. The remaining cells are reserved. At runtime
the renderer interpolates the four channels along local Z and combines the
authored density with analytic ray/ellipsoid depth, so the result remains a
volume rather than a camera-facing replacement card.
