#!/usr/bin/env python3
"""Export MOHU RSC/TIM localization assets with exact provenance."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath

from PIL import Image


@dataclass(frozen=True)
class TimRecord:
    index: int
    offset: int
    span: int
    flags: int
    mode: int
    bits_per_pixel: int
    clut_offset: int | None
    clut_size: int
    clut_x: int
    clut_y: int
    clut_width: int
    clut_height: int
    image_block_offset: int
    image_size: int
    vram_x: int
    vram_y: int
    word_width: int
    pixel_width: int
    height: int


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def category_for(iso_path: str) -> str:
    upper = iso_path.upper()
    if "/SCR2/MBRIEF/" in upper:
        return "BRIEFING"
    if "/SCR3/" in upper:
        return "DOSSIER"
    if "/SCR1/" in upper or "/SCR2/" in upper:
        return "MENU"
    return "COMMON"


def list_resources(sf_tool: Path, cue: Path) -> list[tuple[str, int]]:
    result = subprocess.run(
        [str(sf_tool), "list-files", str(cue)],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    rows = csv.DictReader(result.stdout.splitlines())
    resources = []
    for row in rows:
        iso_path = "/" + row["path"].strip().lstrip("/")
        upper = iso_path.upper()
        if not upper.endswith(".RSC"):
            continue
        if not (
            upper in {"/DATA/GLOBAL.RSC", "/DATA/GLOBAL2.RSC"}
            or upper.startswith("/DATA/SCR1/")
            or upper.startswith("/DATA/SCR2/")
            or upper.startswith("/DATA/SCR3/")
        ):
            continue
        resources.append((iso_path, int(row["size"])))
    return sorted(resources)


def output_path(root: Path, iso_path: str) -> Path:
    relative = PurePosixPath(iso_path.lstrip("/"))
    if ".." in relative.parts:
        raise ValueError(f"Unsafe ISO path: {iso_path}")
    return root.joinpath(*relative.parts)


def extract_resource(
    sf_tool: Path, cue: Path, rsc_root: Path, item: tuple[str, int]
) -> tuple[str, Path]:
    iso_path, expected_size = item
    target = output_path(rsc_root, iso_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file() and target.stat().st_size == expected_size:
        return iso_path, target
    subprocess.run(
        [str(sf_tool), "extract-file", str(cue), iso_path, str(target)],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if target.stat().st_size != expected_size:
        raise RuntimeError(f"Wrong extracted size for {iso_path}")
    return iso_path, target


def scan_tim(data: bytes) -> list[TimRecord]:
    records: list[TimRecord] = []
    for offset in range(0, max(0, len(data) - 19), 4):
        if u32(data, offset) != 0x10:
            continue
        flags = u32(data, offset + 4)
        mode = flags & 7
        if mode > 3 or flags & ~0xF:
            continue
        position = offset + 8
        clut_offset = None
        clut_size = clut_x = clut_y = clut_width = clut_height = 0
        if flags & 8:
            if position + 12 > len(data):
                continue
            clut_offset = position
            clut_size = u32(data, position)
            if clut_size < 12 or position + clut_size > len(data):
                continue
            clut_x, clut_y = u16(data, position + 4), u16(data, position + 6)
            clut_width, clut_height = u16(data, position + 8), u16(data, position + 10)
            if not clut_width or not clut_height:
                continue
            if clut_size != 12 + clut_width * clut_height * 2:
                continue
            position += clut_size
        if position + 12 > len(data):
            continue
        image_size = u32(data, position)
        if image_size < 12 or position + image_size > len(data):
            continue
        vram_x, vram_y = u16(data, position + 4), u16(data, position + 6)
        word_width, height = u16(data, position + 8), u16(data, position + 10)
        if not word_width or not height or image_size != 12 + word_width * height * 2:
            continue
        pixel_width = (word_width * 4, word_width * 2, word_width, word_width * 2 // 3)[mode]
        if not pixel_width or pixel_width > 4096 or height > 1024:
            continue
        records.append(
            TimRecord(
                index=len(records),
                offset=offset,
                span=position + image_size - offset,
                flags=flags,
                mode=mode,
                bits_per_pixel=(4, 8, 16, 24)[mode],
                clut_offset=clut_offset,
                clut_size=clut_size,
                clut_x=clut_x,
                clut_y=clut_y,
                clut_width=clut_width,
                clut_height=clut_height,
                image_block_offset=position,
                image_size=image_size,
                vram_x=vram_x,
                vram_y=vram_y,
                word_width=word_width,
                pixel_width=pixel_width,
                height=height,
            )
        )
    return records


def rgba555(value: int) -> tuple[int, int, int, int]:
    return (
        (value & 31) * 255 // 31,
        ((value >> 5) & 31) * 255 // 31,
        ((value >> 10) & 31) * 255 // 31,
        0 if value == 0 else 255,
    )


def decode_tim(data: bytes, record: TimRecord) -> Image.Image:
    raw = data[record.image_block_offset + 12 : record.image_block_offset + record.image_size]
    image = Image.new("RGBA", (record.pixel_width, record.height))
    if record.mode in (0, 1):
        assert record.clut_offset is not None
        palette_count = 16 if record.mode == 0 else 256
        palette_start = record.clut_offset + 12
        palette = [rgba555(u16(data, palette_start + index * 2)) for index in range(palette_count)]
        indices: list[int] = []
        if record.mode == 0:
            for value in raw:
                indices.extend((value & 15, value >> 4))
        else:
            indices = list(raw)
        image.putdata([palette[index] for index in indices[: record.pixel_width * record.height]])
        return image
    if record.mode == 2:
        image.putdata([rgba555(u16(raw, index)) for index in range(0, len(raw), 2)])
        return image
    pixels = []
    row_bytes = record.word_width * 2
    for y in range(record.height):
        row = raw[y * row_bytes : (y + 1) * row_bytes]
        for x in range(record.pixel_width):
            base = x * 3
            pixels.append((row[base], row[base + 1], row[base + 2], 255))
    image.putdata(pixels)
    return image


def write_tsv(path: Path, fieldnames: list[str], rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, dialect="excel-tab", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def export(args: argparse.Namespace) -> None:
    sf_tool = args.sf_tool.resolve()
    cue = args.cue.resolve()
    root = args.output.resolve()
    if not sf_tool.is_file() or not cue.is_file():
        raise FileNotFoundError("sf_tool or CUE not found")
    rsc_root, tim_root, page_root = root / "rsc", root / "tim", root / "pages"
    resources = list_resources(sf_tool, cue)
    print(f"RSC resources: {len(resources)}")
    extracted: dict[str, Path] = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(extract_resource, sf_tool, cue, rsc_root, item) for item in resources]
        for done, future in enumerate(as_completed(futures), 1):
            iso_path, path = future.result()
            extracted[iso_path] = path
            if done % 20 == 0 or done == len(futures):
                print(f"Extracted: {done}/{len(futures)}")

    rsc_rows: list[dict[str, object]] = []
    tim_rows: list[dict[str, object]] = []
    page_rows: list[dict[str, object]] = []
    review_rows: list[dict[str, object]] = []
    for iso_path, expected_size in resources:
        path = extracted[iso_path]
        data = path.read_bytes()
        records = scan_tim(data)
        category = category_for(iso_path)
        rsc_rows.append(
            {"iso_path": iso_path, "category": category, "size": expected_size, "sha256": sha256(data), "tim_count": len(records)}
        )
        resource_rel = PurePosixPath(iso_path.lstrip("/")).with_suffix("")
        asset_dir = tim_root.joinpath(*resource_rel.parts)
        images: dict[int, Image.Image] = {}
        for record in records:
            image = decode_tim(data, record)
            images[record.index] = image
            name = f"tim_{record.index:03d}_off_{record.offset:08x}_{record.pixel_width}x{record.height}.png"
            png = asset_dir / name
            png.parent.mkdir(parents=True, exist_ok=True)
            image.save(png, optimize=True)
            row = asdict(record)
            row.update(
                iso_path=iso_path,
                category=category,
                rsc_sha256=sha256(data),
                tim_sha256=sha256(data[record.offset : record.offset + record.span]),
                png=png.relative_to(root).as_posix(),
            )
            tim_rows.append(row)

        candidates = [r for r in records if r.pixel_width == 256 and r.height == 240]
        unused = set(r.index for r in candidates)
        pairs: list[tuple[TimRecord, TimRecord]] = []
        for left in sorted(candidates, key=lambda r: (r.vram_y, r.vram_x, r.offset)):
            if left.index not in unused:
                continue
            right = next(
                (
                    r
                    for r in candidates
                    if r.index in unused
                    and r.index != left.index
                    and r.mode == left.mode
                    and r.vram_y == left.vram_y
                    and r.vram_x == left.vram_x + left.word_width
                ),
                None,
            )
            if right is None:
                continue
            unused.remove(left.index)
            unused.remove(right.index)
            pairs.append((left, right))
        if category == "BRIEFING":
            remaining = sorted((r for r in candidates if r.index in unused), key=lambda r: r.offset)
            for position in range(0, len(remaining) - 1, 2):
                left, right = remaining[position : position + 2]
                if (
                    left.vram_x == right.vram_x
                    and left.vram_y == right.vram_y
                    and left.mode == right.mode
                ):
                    pairs.append((left, right))
        pairs.sort(key=lambda pair: min(pair[0].offset, pair[1].offset))
        for page_index, (left, right) in enumerate(pairs):
            page = Image.new("RGBA", (512, 240), (0, 0, 0, 255))
            page.alpha_composite(images[left.index], (0, 0))
            page.alpha_composite(images[right.index], (256, 0))
            name = f"page_{page_index:03d}_offs_{left.offset:08x}_{right.offset:08x}.png"
            png = page_root.joinpath(*resource_rel.parts, name)
            png.parent.mkdir(parents=True, exist_ok=True)
            page.convert("RGB").save(png, optimize=True)
            page_id = f"MOHU-{category[:3]}-{len(page_rows) + 1:04d}"
            page_rows.append(
                {
                    "id": page_id,
                    "iso_path": iso_path,
                    "category": category,
                    "page_index": page_index,
                    "left_tim_offset": f"0x{left.offset:08x}",
                    "right_tim_offset": f"0x{right.offset:08x}",
                    "png": png.relative_to(root).as_posix(),
                }
            )
            review_rows.append(
                {
                    "id": page_id,
                    "category": category,
                    "iso_path": iso_path,
                    "asset": png.relative_to(root).as_posix(),
                    "english": "",
                    "russian": "",
                    "status": "NEEDS_TRANSCRIPTION",
                    "comment": "",
                }
            )

    write_tsv(root / "rsc_manifest.tsv", ["iso_path", "category", "size", "sha256", "tim_count"], rsc_rows)
    tim_fields = ["iso_path", "category", "rsc_sha256", "index", "offset", "span", "flags", "bits_per_pixel", "clut_offset", "clut_size", "clut_x", "clut_y", "clut_width", "clut_height", "image_block_offset", "image_size", "vram_x", "vram_y", "word_width", "pixel_width", "height", "tim_sha256", "png"]
    write_tsv(root / "tim_manifest.tsv", tim_fields, tim_rows)
    write_tsv(root / "page_manifest.tsv", ["id", "iso_path", "category", "page_index", "left_tim_offset", "right_tim_offset", "png"], page_rows)
    write_tsv(root / "translation_review.tsv", ["id", "category", "iso_path", "asset", "english", "russian", "status", "comment"], review_rows)
    manifest = {"cue": str(cue), "resources": rsc_rows, "tim_count": len(tim_rows), "page_count": len(page_rows)}
    (root / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    summary = f"RSC: {len(rsc_rows)}\nTIM: {len(tim_rows)}\nPAGES: {len(page_rows)}\n"
    (root / "README.txt").write_text(summary, encoding="utf-8-sig")
    print(summary, end="")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cue", type=Path, required=True)
    parser.add_argument("--sf-tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    return parser.parse_args()


if __name__ == "__main__":
    try:
        export(parse_args())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
