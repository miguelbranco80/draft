#!/usr/bin/env python3
"""Generate Draft interval tables from one pinned Unicode Character Database.

This maintenance tool is not part of a Draft package: draftc selects only
direct `.draft` sources. Pass the five Unicode 17.0.0 files named below and
redirect stdout to `data.draft`. The generated header records their SHA-256
digests, so reviewing a regenerated table never depends on ambient "latest"
Unicode data.
"""

from __future__ import annotations

import argparse
import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class Range:
    first: int
    last: int
    value: str = ""


def parse_range(text: str) -> tuple[int, int]:
    fields = text.strip().split("..")
    first = int(fields[0], 16)
    return first, int(fields[1], 16) if len(fields) == 2 else first


def merged(ranges: Iterable[Range]) -> list[Range]:
    result: list[Range] = []
    for current in sorted(ranges, key=lambda item: (item.first, item.last)):
        if result and result[-1].value == current.value and (
            result[-1].last + 1 == current.first
        ):
            previous = result[-1]
            result[-1] = Range(previous.first, current.last, previous.value)
        else:
            result.append(current)
    return result


def parse_property(path: Path, wanted: set[str]) -> list[Range]:
    result: list[Range] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        fields = [field.strip() for field in body.split(";")]
        property_name = fields[-1]
        if property_name not in wanted:
            continue
        first, last = parse_range(fields[0])
        result.append(Range(first, last, property_name))
    return merged(result)


def parse_unicode_categories(path: Path, wanted: set[str]) -> list[Range]:
    result: list[Range] = []
    pending_first: tuple[int, str] | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split(";")
        codepoint = int(fields[0], 16)
        name = fields[1]
        category = fields[2]
        if name.endswith(", First>"):
            pending_first = (codepoint, category)
            continue
        if name.endswith(", Last>"):
            if pending_first is None or pending_first[1] != category:
                raise ValueError(f"unmatched UnicodeData range end: {line}")
            if category in wanted:
                result.append(Range(pending_first[0], codepoint, category))
            pending_first = None
            continue
        if category in wanted:
            result.append(Range(codepoint, codepoint, category))
    if pending_first is not None:
        raise ValueError("unterminated UnicodeData First/Last range")
    return merged(result)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hex_value(value: int) -> str:
    return f"0x{value:04x}" if value <= 0xFFFF else f"0x{value:06x}"


def emit_array(name: str, type_name: str, ranges: list[Range], values: dict[str, int] | None = None) -> None:
    # The package helpers borrow these tables as slices, so the generated data
    # needs static storage rather than Draft's addressless constant semantics.
    # The names remain package-private and package.draft never mutates them.
    print(f"{name}: [{len(ranges)}]{type_name} = [{len(ranges)}]{type_name}{{")
    for item in ranges:
        suffix = ""
        if values is not None:
            suffix = f", property = {values[item.value]}"
        print(
            f"    {type_name}{{first = {hex_value(item.first)}, "
            f"last = {hex_value(item.last)}{suffix}}},"
        )
    print("}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("unicode_data", type=Path)
    parser.add_argument("grapheme_break", type=Path)
    parser.add_argument("derived_core", type=Path)
    parser.add_argument("emoji_data", type=Path)
    parser.add_argument("east_asian_width", type=Path)
    args = parser.parse_args()

    grapheme_names = {
        "CR": 1,
        "LF": 2,
        "Control": 3,
        "Extend": 4,
        "ZWJ": 5,
        "Regional_Indicator": 6,
        "Prepend": 7,
        "SpacingMark": 8,
        "L": 9,
        "V": 10,
        "T": 11,
        # The 11,172 modern Hangul syllables alternate algorithmically between
        # LV and LVT. package.draft computes that property directly, avoiding
        # hundreds of generated one/twenty-seven-syllable intervals.
    }
    indic_names = {
        "Consonant": 1,
        "Linker": 2,
        "Extend": 3,
    }

    grapheme = parse_property(args.grapheme_break, set(grapheme_names))
    indic = parse_property(args.derived_core, set(indic_names))
    extended_pictographic = parse_property(args.emoji_data, {"Extended_Pictographic"})
    emoji_presentation = parse_property(args.emoji_data, {"Emoji_Presentation"})
    wide = parse_property(args.east_asian_width, {"W", "F"})
    controls = parse_unicode_categories(args.unicode_data, {"Cc"})
    zero_width = parse_unicode_categories(args.unicode_data, {"Mn", "Me", "Cf"})

    paths = [
        args.unicode_data,
        args.grapheme_break,
        args.derived_core,
        args.emoji_data,
        args.east_asian_width,
    ]
    print("// Generated Unicode 17.0.0 interval tables. Do not edit by hand.")
    print("// Sources: https://www.unicode.org/Public/17.0.0/ucd/")
    for path in paths:
        print(f"// SHA-256 {path.name}: {digest(path)}")
    print("// Unicode data terms: https://www.unicode.org/terms_of_use.html")
    print("package unicode\n")

    emit_array("Grapheme_Ranges", "Property_Range", grapheme, grapheme_names)
    print()
    emit_array("Indic_Ranges", "Indic_Range", indic, indic_names)
    print()
    emit_array("Control_Ranges", "Scalar_Range", controls)
    print()
    emit_array("Zero_Width_Ranges", "Scalar_Range", zero_width)
    print()
    emit_array("Wide_Ranges", "Scalar_Range", wide)
    print()
    emit_array("Extended_Pictographic_Ranges", "Scalar_Range", extended_pictographic)
    print()
    emit_array("Emoji_Presentation_Ranges", "Scalar_Range", emoji_presentation)


if __name__ == "__main__":
    main()
