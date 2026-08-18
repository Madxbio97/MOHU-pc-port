#!/usr/bin/env python3
"""Repack edited MOHU TIM PNGs into size-stable RSC files."""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from collections import defaultdict
from pathlib import Path, PurePosixPath

from PIL import Image

from export_mohu_localization import TimRecord, rgba555, scan_tim, sha256, u16


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, dialect="excel-tab"))


def iso_output_path(root: Path, iso_path: str) -> Path:
    relative = PurePosixPath(iso_path.lstrip("/"))
    if ".." in relative.parts:
        raise ValueError(f"Unsafe ISO path: {iso_path}")
    return root.joinpath(*relative.parts)


def encode_555(pixel: tuple[int, int, int, int]) -> int:
    red, green, blue, alpha = pixel
    if alpha < 128:
        return 0
    value = (
        ((red * 31 + 127) // 255)
        | (((green * 31 + 127) // 255) << 5)
        | (((blue * 31 + 127) // 255) << 10)
    )
    return value if value else 0x8000


def source_indices(data: bytes, record: TimRecord) -> list[int]:
    raw = data[
        record.image_block_offset + 12 : record.image_block_offset + record.image_size
    ]
    if record.mode == 0:
        indices: list[int] = []
        for value in raw:
            indices.extend((value & 15, value >> 4))
        return indices[: record.pixel_width * record.height]
    if record.mode == 1:
        return list(raw)
    raise ValueError("Indexed source requested for direct-color TIM")


def pack_indices(indices: list[int], mode: int) -> bytes:
    if mode == 1:
        return bytes(indices)
    packed = bytearray((len(indices) + 1) // 2)
    for position in range(0, len(indices), 2):
        low = indices[position]
        high = indices[position + 1] if position + 1 < len(indices) else 0
        packed[position // 2] = low | (high << 4)
    return bytes(packed)


def encode_indexed(
    original: bytes, output: bytearray, record: TimRecord, image: Image.Image
) -> None:
    if record.clut_offset is None:
        raise ValueError("Indexed TIM has no CLUT")
    palette_count = 16 if record.mode == 0 else 256
    palette_start = record.clut_offset + 12
    palette = [u16(original, palette_start + index * 2) for index in range(palette_count)]
    decoded = [rgba555(value) for value in palette]
    original_indices = source_indices(original, record)
    used = set(original_indices)
    free = [index for index in range(palette_count - 1, 0, -1) if index not in used]
    rgba_map: dict[tuple[int, int, int, int], int] = {}
    word_map: dict[int, int] = {}
    for index, value in enumerate(palette):
        rgba_map.setdefault(decoded[index], index)
        word_map.setdefault(value, index)

    replacement_indices: list[int] = []
    for position, pixel in enumerate(image.get_flattened_data()):
        source_index = original_indices[position]
        if pixel == decoded[source_index]:
            replacement_indices.append(source_index)
            continue
        existing = rgba_map.get(pixel)
        if existing is not None:
            replacement_indices.append(existing)
            continue
        word = encode_555(pixel)
        existing = word_map.get(word)
        if existing is not None:
            replacement_indices.append(existing)
            continue
        if not free:
            candidates = range(1, palette_count) if pixel[3] >= 128 else range(palette_count)
            index = min(
                candidates,
                key=lambda candidate: sum(
                    (pixel[channel] - decoded[candidate][channel]) ** 2
                    for channel in range(4)
                ),
            )
            rgba_map[pixel] = index
            replacement_indices.append(index)
            continue
        index = free.pop()
        palette[index] = word
        decoded[index] = rgba555(word)
        rgba_map[pixel] = index
        word_map[word] = index
        replacement_indices.append(index)

    for index, value in enumerate(palette):
        struct.pack_into("<H", output, palette_start + index * 2, value)
    raw = pack_indices(replacement_indices, record.mode)
    expected = record.image_size - 12
    if len(raw) != expected:
        raise ValueError(
            f"TIM 0x{record.offset:08x} encoded {len(raw)} bytes, expected {expected}"
        )
    start = record.image_block_offset + 12
    output[start : start + expected] = raw


def encode_16bit(
    original: bytes, output: bytearray, record: TimRecord, image: Image.Image
) -> None:
    start = record.image_block_offset + 12
    pixels = list(image.get_flattened_data())
    for position, pixel in enumerate(pixels):
        offset = start + position * 2
        source_value = u16(original, offset)
        value = source_value if pixel == rgba555(source_value) else encode_555(pixel)
        struct.pack_into("<H", output, offset, value)


def encode_24bit(
    original: bytes, output: bytearray, record: TimRecord, image: Image.Image
) -> None:
    start = record.image_block_offset + 12
    row_bytes = record.word_width * 2
    pixels = list(image.getdata())
    for y in range(record.height):
        for x in range(record.pixel_width):
            pixel = pixels[y * record.pixel_width + x]
            offset = start + y * row_bytes + x * 3
            source_pixel = tuple(original[offset : offset + 3]) + (255,)
            if pixel != source_pixel:
                output[offset : offset + 3] = bytes(pixel[:3])


def apply_png(
    original: bytes, output: bytearray, record: TimRecord, png: Path
) -> None:
    with Image.open(png) as source_image:
        image = source_image.convert("RGBA")
    expected_size = (record.pixel_width, record.height)
    if image.size != expected_size:
        raise ValueError(
            f"{png}: size {image.size[0]}x{image.size[1]}, expected "
            f"{expected_size[0]}x{expected_size[1]}"
        )
    if record.mode in (0, 1):
        encode_indexed(original, output, record, image)
    elif record.mode == 2:
        encode_16bit(original, output, record, image)
    else:
        encode_24bit(original, output, record, image)


def write_manifest(path: Path, rows: list[dict[str, object]]) -> None:
    fields = ["iso_path", "source_sha256", "output_sha256", "replacement_count", "output"]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, dialect="excel-tab")
        writer.writeheader()
        writer.writerows(rows)


def run(args: argparse.Namespace) -> None:
    export_root = args.export_root.resolve()
    rsc_manifest = {
        row["iso_path"]: row for row in read_tsv(export_root / "rsc_manifest.tsv")
    }
    tim_rows: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in read_tsv(export_root / "tim_manifest.tsv"):
        tim_rows[row["iso_path"]].append(row)

    replacement_root = (
        export_root if args.verify_roundtrip else args.replacement_root.resolve()
    )
    output_root = args.output_root.resolve()
    results: list[dict[str, object]] = []
    total_replacements = 0
    verified = 0

    for iso_path, resource in sorted(rsc_manifest.items()):
        source_path = iso_output_path(export_root / "rsc", iso_path)
        original = source_path.read_bytes()
        if sha256(original) != resource["sha256"]:
            raise ValueError(f"Source hash mismatch: {iso_path}")
        records = {record.offset: record for record in scan_tim(original)}
        replacements: list[tuple[TimRecord, Path]] = []
        for row in tim_rows.get(iso_path, []):
            png = replacement_root / row["png"]
            if not png.is_file():
                if args.verify_roundtrip:
                    raise FileNotFoundError(png)
                continue
            offset = int(row["offset"])
            record = records.get(offset)
            if record is None:
                raise ValueError(f"TIM offset 0x{offset:08x} missing in {iso_path}")
            replacements.append((record, png))
        if not replacements and not args.verify_roundtrip:
            continue

        output = bytearray(original)
        for record, png in replacements:
            apply_png(original, output, record, png)
        output_bytes = bytes(output)
        total_replacements += len(replacements)

        if args.verify_roundtrip:
            if output_bytes != original:
                mismatch = next(
                    index
                    for index, pair in enumerate(zip(output_bytes, original))
                    if pair[0] != pair[1]
                )
                raise ValueError(
                    f"Round-trip mismatch: {iso_path} at 0x{mismatch:08x}"
                )
            verified += len(replacements)
            continue

        target = iso_output_path(output_root, iso_path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(output_bytes)
        results.append(
            {
                "iso_path": iso_path,
                "source_sha256": sha256(original),
                "output_sha256": sha256(output_bytes),
                "replacement_count": len(replacements),
                "output": target.relative_to(output_root).as_posix(),
            }
        )

    if args.verify_roundtrip:
        print(f"Round-trip verified: {verified} TIM in {len(rsc_manifest)} RSC")
        return
    write_manifest(output_root / "import_manifest.tsv", results)
    print(f"RSC written: {len(results)}")
    print(f"TIM replaced: {total_replacements}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export-root", type=Path, required=True)
    parser.add_argument("--replacement-root", type=Path)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--verify-roundtrip", action="store_true")
    args = parser.parse_args()
    if not args.verify_roundtrip and args.replacement_root is None:
        parser.error("--replacement-root is required unless --verify-roundtrip is used")
    return args


if __name__ == "__main__":
    try:
        run(parse_args())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
