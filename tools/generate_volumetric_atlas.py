#!/usr/bin/env python3
"""Build the authored four-slice volumetric density atlas and C++ header."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageEnhance, ImageFilter, ImageOps


CELL_SIZE = 128
COLUMNS = 4
ROWS = 2
CONTENT_SIZE = 112
EFFECTS = ("fire", "explosion", "smoke", "fog", "halo")


@dataclass(frozen=True)
class SliceSpec:
    scale: float
    intensity: float
    blur: float
    offset_x: int = 0
    offset_y: int = 0


SLICE_SPECS = {
    "fire": (
        SliceSpec(0.76, 0.68, 1.5, -2, 1),
        SliceSpec(0.98, 1.00, 0.35, 0, 0),
        SliceSpec(0.93, 0.91, 0.65, 1, -1),
        SliceSpec(0.72, 0.62, 1.8, 2, 1),
    ),
    "explosion": (
        SliceSpec(0.75, 0.64, 1.8, -1, 1),
        SliceSpec(0.98, 1.00, 0.45, 0, 0),
        SliceSpec(0.94, 0.92, 0.75, 1, -1),
        SliceSpec(0.72, 0.60, 2.0, 2, 1),
    ),
    "smoke": (
        SliceSpec(0.80, 0.65, 1.7, -2, 1),
        SliceSpec(0.98, 1.00, 0.55, 0, 0),
        SliceSpec(0.94, 0.88, 0.95, 2, -1),
        SliceSpec(0.77, 0.61, 1.9, 1, 2),
    ),
    "fog": (
        SliceSpec(0.86, 0.68, 1.8, -2, 0),
        SliceSpec(1.00, 1.00, 0.65, 0, 0),
        SliceSpec(0.97, 0.91, 1.0, 2, 0),
        SliceSpec(0.83, 0.65, 2.1, 1, 1),
    ),
    "halo": (
        SliceSpec(0.78, 0.66, 1.8),
        SliceSpec(1.00, 1.00, 0.45),
        SliceSpec(0.96, 0.91, 0.8),
        SliceSpec(0.76, 0.63, 2.0),
    ),
}


def density_image(path: Path, effect: str) -> Image.Image:
    source = Image.open(path).convert("RGB")
    if effect in {"fire", "explosion", "halo"}:
        red, green, blue = source.split()
        density = ImageChops.lighter(red, ImageChops.lighter(green, blue))
        gamma = 0.82 if effect != "halo" else 0.72
    else:
        density = ImageOps.grayscale(source)
        gamma = 0.92 if effect == "smoke" else 0.86

    threshold = 3 if effect != "fog" else 2
    linear = [
        0
        if value <= threshold
        else round((value - threshold) * 255 / (255 - threshold))
        for value in range(256)
    ]
    density = density.point(linear)
    density = density.point(
        [round(pow(value / 255.0, gamma) * 255.0) for value in range(256)]
    )
    visible = density.point(lambda value: 255 if value > 2 else 0)
    bounds = visible.getbbox()
    if bounds is None:
        raise ValueError(f"{path} contains no visible {effect} density")

    left, top, right, bottom = bounds
    margin = max(4, round(max(right - left, bottom - top) * 0.06))
    left = max(0, left - margin)
    top = max(0, top - margin)
    right = min(density.width, right + margin)
    bottom = min(density.height, bottom + margin)
    cropped = density.crop((left, top, right, bottom))
    scale = min(CONTENT_SIZE / cropped.width, CONTENT_SIZE / cropped.height)
    size = (
        max(1, round(cropped.width * scale)),
        max(1, round(cropped.height * scale)),
    )
    cropped = cropped.resize(size, Image.Resampling.LANCZOS)
    cell = Image.new("L", (CELL_SIZE, CELL_SIZE), 0)
    cell.paste(cropped, ((CELL_SIZE - size[0]) // 2, (CELL_SIZE - size[1]) // 2))
    return cell


def transform_slice(source: Image.Image, spec: SliceSpec) -> Image.Image:
    size = max(1, round(CELL_SIZE * spec.scale))
    resized = source.resize((size, size), Image.Resampling.LANCZOS)
    result = Image.new("L", (CELL_SIZE, CELL_SIZE), 0)
    result.paste(
        resized,
        (
            (CELL_SIZE - size) // 2 + spec.offset_x,
            (CELL_SIZE - size) // 2 + spec.offset_y,
        ),
    )
    if spec.blur > 0.0:
        result = result.filter(ImageFilter.GaussianBlur(spec.blur))
    return ImageEnhance.Brightness(result).enhance(spec.intensity)


def maximum_channel(tile: Image.Image) -> Image.Image:
    red, green, blue, alpha = tile.split()
    return ImageChops.lighter(
        ImageChops.lighter(red, green), ImageChops.lighter(blue, alpha)
    )


def format_bytes(values: bytes, per_line: int = 16) -> str:
    lines = []
    for offset in range(0, len(values), per_line):
        row = ", ".join(f"0x{value:02x}U" for value in values[offset : offset + per_line])
        lines.append(f"    {row},")
    return "\n".join(lines)


def build(source_dir: Path, atlas_path: Path, preview_path: Path, header_path: Path) -> None:
    atlas = Image.new("RGBA", (CELL_SIZE * COLUMNS, CELL_SIZE * ROWS), 0)
    preview = Image.new("L", atlas.size, 0)
    for index, effect in enumerate(EFFECTS):
        source = density_image(source_dir / f"{effect}.png", effect)
        slices = [transform_slice(source, spec) for spec in SLICE_SPECS[effect]]
        tile = Image.merge("RGBA", slices)
        x = (index % COLUMNS) * CELL_SIZE
        y = (index // COLUMNS) * CELL_SIZE
        atlas.paste(tile, (x, y))
        preview.paste(maximum_channel(tile), (x, y))

    atlas_path.parent.mkdir(parents=True, exist_ok=True)
    preview_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(atlas_path, optimize=True)
    preview.save(preview_path, optimize=True)

    # OpenGL consumes the first upload row as the lower edge. Flip here so
    # shader UVs retain the source art's ordinary top-to-bottom orientation.
    upload = atlas.transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes()
    header = f"""#pragma once

#include <array>
#include <cstdint>

namespace sf::platform::detail::volumetric_atlas_texture {{

inline constexpr std::uint16_t width = {atlas.width}U;
inline constexpr std::uint16_t height = {atlas.height}U;
inline constexpr std::uint16_t cell_size = {CELL_SIZE}U;
inline constexpr std::uint8_t columns = {COLUMNS}U;
inline constexpr std::uint8_t rows = {ROWS}U;

// RGBA stores density at local Z = -0.75, -0.25, +0.25 and +0.75.
inline constexpr std::array<std::uint8_t, {len(upload)}U> rgba{{{{
{format_bytes(upload)}
}}}};

}} // namespace sf::platform::detail::volumetric_atlas_texture
"""
    header_path.write_text(header, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=Path)
    parser.add_argument("atlas", type=Path)
    parser.add_argument("preview", type=Path)
    parser.add_argument("header", type=Path)
    arguments = parser.parse_args()
    build(arguments.source_dir, arguments.atlas, arguments.preview, arguments.header)


if __name__ == "__main__":
    main()
