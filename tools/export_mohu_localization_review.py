#!/usr/bin/env python3
"""Create editable EN/RU review text files from a MOHU localization export."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path


FIELDS = [
    "id",
    "category",
    "asset_kind",
    "iso_path",
    "provenance",
    "image",
    "source_en",
    "translation_ru",
    "status",
    "comment",
]


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, dialect="excel-tab"))


def write_tsv(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS, dialect="excel-tab")
        writer.writeheader()
        writer.writerows(rows)


def write_text(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8-sig", newline="\n") as stream:
        for row in rows:
            stream.write(f"[{row['id']}] {row['category']} / {row['asset_kind']}\n")
            stream.write(f"ISO: {row['iso_path']}\n")
            stream.write(f"PROVENANCE: {row['provenance']}\n")
            stream.write(f"IMAGE: {row['image']}\n")
            stream.write(f"EN: {row['source_en']}\n")
            stream.write(f"RU: {row['translation_ru']}\n")
            stream.write(f"STATUS: {row['status']}\n")
            stream.write(f"COMMENT: {row['comment']}\n\n")


def build_rows(root: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for tim in read_tsv(root / "tim_manifest.tsv"):
        rows.append(
            {
                "category": tim["category"],
                "asset_kind": "TIM",
                "iso_path": tim["iso_path"],
                "provenance": (
                    f"rsc_sha256={tim['rsc_sha256']}; tim_offset=0x{int(tim['offset']):08x}; "
                    f"tim_sha256={tim['tim_sha256']}; {tim['bits_per_pixel']}bpp; "
                    f"{tim['pixel_width']}x{tim['height']}; vram={tim['vram_x']},{tim['vram_y']}"
                ),
                "image": tim["png"],
                "source_en": "",
                "translation_ru": "",
                "status": "NEEDS_CLASSIFICATION",
                "comment": "",
            }
        )
    for page in read_tsv(root / "page_manifest.tsv"):
        rows.append(
            {
                "category": page["category"],
                "asset_kind": "PAGE",
                "iso_path": page["iso_path"],
                "provenance": (
                    f"left_tim_offset={page['left_tim_offset']}; "
                    f"right_tim_offset={page['right_tim_offset']}"
                ),
                "image": page["png"],
                "source_en": "",
                "translation_ru": "",
                "status": "NEEDS_TRANSCRIPTION",
                "comment": "",
            }
        )
    rows.sort(key=lambda row: (row["category"], row["iso_path"], row["asset_kind"], row["image"]))
    counters: dict[tuple[str, str], int] = defaultdict(int)
    category_codes = {"BRIEFING": "BRF", "MENU": "MNU", "DOSSIER": "DOS", "COMMON": "COM"}
    for row in rows:
        key = (row["category"], row["asset_kind"])
        counters[key] += 1
        row["id"] = (
            f"MOHU-{category_codes[row['category']]}-{row['asset_kind'][:1]}-"
            f"{counters[key]:04d}"
        )
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.export_root.resolve()
    review_root = root / "review"
    review_root.mkdir(parents=True, exist_ok=True)
    rows = build_rows(root)
    groups = {
        "all": rows,
        "briefings": [row for row in rows if row["category"] == "BRIEFING"],
        "menus": [row for row in rows if row["category"] in {"MENU", "COMMON"}],
        "dossiers": [row for row in rows if row["category"] == "DOSSIER"],
    }
    for name, group in groups.items():
        write_tsv(review_root / f"{name}_review.tsv", group)
        write_text(review_root / f"{name}_review.txt", group)
    print(f"Review rows: {len(rows)}")
    for name, group in groups.items():
        print(f"{name}: {len(group)}")


if __name__ == "__main__":
    main()
