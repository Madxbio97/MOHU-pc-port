#!/usr/bin/env python3
"""Build the Russian MOPTION.RSC pilot replacement textures."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


LABELS = {
    "0001501c": "Аудио",
    "00015fec": "Управление",
    "00016fbc": "Сложность",
    "00017f8c": "Пароли",
    "00018f5c": "Секреты",
}


def find_tim(root: Path, offset: str) -> Path:
    matches = list(root.glob(f"*off_{offset}_*.png"))
    if len(matches) != 1:
        raise RuntimeError(f"Expected one TIM for offset {offset}, found {len(matches)}")
    return matches[0]


def text_layer(
    text: str,
    font_path: Path,
    font_size: int,
    color: tuple[int, int, int, int],
    vertical: bool,
) -> Image.Image:
    scale = 4
    font = ImageFont.truetype(str(font_path), font_size * scale)
    probe = Image.new("RGBA", (1, 1))
    bounds = ImageDraw.Draw(probe).textbbox(
        (0, 0), text, font=font, stroke_width=scale // 2
    )
    width = bounds[2] - bounds[0] + scale * 2
    height = bounds[3] - bounds[1] + scale * 2
    layer = Image.new("RGBA", (width, height))
    draw = ImageDraw.Draw(layer)
    origin = (scale - bounds[0], scale - bounds[1])
    draw.text(
        (origin[0] + scale // 2, origin[1] + scale // 2),
        text,
        font=font,
        fill=(0, 0, 0, 190),
        stroke_width=scale // 2,
        stroke_fill=(0, 0, 0, 190),
    )
    draw.text(
        origin,
        text,
        font=font,
        fill=color,
        stroke_width=scale // 3,
        stroke_fill=(72, 73, 39, 220),
    )
    if vertical:
        layer = layer.rotate(90, expand=True, resample=Image.Resampling.BICUBIC)
    size = (
        max(1, round(layer.width / scale)),
        max(1, round(layer.height / scale)),
    )
    return layer.resize(size, Image.Resampling.LANCZOS)


def erase_bright_text(
    image: Image.Image,
    box: tuple[int, int, int, int],
    threshold: int,
    blur_radius: float,
) -> None:
    region = image.crop(box)
    blurred = region.filter(ImageFilter.GaussianBlur(blur_radius))
    mask = Image.new("L", region.size)
    source = region.convert("RGB")
    mask.putdata(
        [
            255 if max(red, green, blue) >= threshold else 0
            for red, green, blue in source.get_flattened_data()
        ]
    )
    mask = mask.filter(ImageFilter.MaxFilter(5))
    region.paste(blurred, (0, 0), mask)
    image.paste(region, box[:2])


def original_text_metrics(
    image: Image.Image,
) -> tuple[tuple[int, int, int, int], tuple[int, int, int, int]]:
    bounds_pixels: list[tuple[int, int]] = []
    color_pixels: list[tuple[int, int, int, int]] = []
    for y in range(8, 138):
        for x in range(5, 19):
            pixel = image.getpixel((x, y))
            if pixel[0] >= 100 and pixel[1] >= 100 and pixel[2] >= 60:
                bounds_pixels.append((x, y))
            if pixel[0] >= 130 and pixel[1] >= 130 and pixel[2] >= 80:
                color_pixels.append(pixel)
    if not bounds_pixels or not color_pixels:
        raise RuntimeError("Could not measure original menu label")
    left = min(x for x, _ in bounds_pixels)
    top = min(y for _, y in bounds_pixels)
    right = max(x for x, _ in bounds_pixels) + 1
    bottom = max(y for _, y in bounds_pixels) + 1
    color = max(color_pixels, key=lambda pixel: sum(pixel[:3]))
    return (left, top, right, bottom), color


def rebuild_label_interior(image: Image.Image) -> None:
    pixels = image.load()
    for y in range(8, 138):
        samples = [
            pixels[x, y]
            for x in range(5, 19)
            if max(pixels[x, y][:3]) < 82
        ]
        if not samples:
            samples = [pixels[6, y], pixels[17, y]]
        samples.sort(key=lambda pixel: sum(pixel[:3]))
        base = samples[len(samples) // 2]
        for x in range(5, 19):
            distance = abs(x - 11.5) / 7.0
            factor = 1.06 - distance * 0.16
            pixels[x, y] = (
                min(255, round(base[0] * factor)),
                min(255, round(base[1] * factor)),
                min(255, round(base[2] * factor)),
                base[3],
            )


def localize_label(source: Path, target: Path, text: str, font: Path) -> None:
    with Image.open(source) as loaded:
        image = loaded.convert("RGBA")
    target_box, color = original_text_metrics(image)
    rebuild_label_interior(image)
    layer = text_layer(text, font, 11, color, vertical=True)
    alpha_box = layer.getchannel("A").getbbox()
    if alpha_box is None:
        raise RuntimeError(f"Empty rendered label: {text}")
    layer = layer.crop(alpha_box)
    target_size = (target_box[2] - target_box[0], target_box[3] - target_box[1])
    layer = layer.resize(target_size, Image.Resampling.LANCZOS)
    image.alpha_composite(layer, target_box[:2])
    target.parent.mkdir(parents=True, exist_ok=True)
    image.save(target, optimize=True)


def localize_hints(source: Path, target: Path, font: Path) -> None:
    with Image.open(source) as loaded:
        image = loaded.convert("RGBA")
    clean = image.crop((73, 113, 172, 133)).resize(
        (99, 40), Image.Resampling.BICUBIC
    )
    image.paste(clean, (73, 73))
    for text, y in (("Листать", 74), ("Выбрать", 87), ("Назад", 100)):
        layer = text_layer(text, font, 9, (160, 157, 120, 255), vertical=False)
        image.alpha_composite(layer, (82, y))
    target.parent.mkdir(parents=True, exist_ok=True)
    image.save(target, optimize=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--hint-font", type=Path)
    args = parser.parse_args()

    source_root = (
        args.export_root.resolve() / "tim" / "DATA" / "SCR1" / "MOPTION"
    )
    target_root = (
        args.output_root.resolve() / "tim" / "DATA" / "SCR1" / "MOPTION"
    )
    font = args.font.resolve()
    if not font.is_file():
        raise FileNotFoundError(font)
    hint_font = args.hint_font.resolve() if args.hint_font else font
    if not hint_font.is_file():
        raise FileNotFoundError(hint_font)

    for offset, text in LABELS.items():
        source = find_tim(source_root, offset)
        localize_label(source, target_root / source.name, text, font)

    hints = find_tim(source_root, "0001a8cc")
    localize_hints(hints, target_root / hints.name, hint_font)
    print(f"Russian MOPTION textures: {len(LABELS) + 1}")


if __name__ == "__main__":
    main()
