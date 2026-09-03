#!/usr/bin/env python3
"""Generate deterministic development-only Qt pseudo-locales."""

from __future__ import annotations

import argparse
import json
import math
import re
import xml.etree.ElementTree as ET
from pathlib import Path


ACCENTS = str.maketrans(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",
    "ÅƁÇÐËƑǴĦÏĴĶĿḾŃÖÞǪŔŠŦÜṼŴẊŸŽåƀçðëƒǵħïĵķŀḿńöþǫŕšŧüṽŵẋÿž",
)
PLACEHOLDER_RE = re.compile(r"%(?:L?[1-9][0-9]*|n)")
MARKUP_RE = re.compile(r"</?[A-Za-z][^>]*>")
URL_RE = re.compile(r"https?://[^\s<>]+")
PATH_RE = re.compile(r"(?:[A-Za-z]:[\\/]|\\\\)[^\s<>]+")
SHORTCUT_RE = re.compile(r"\b(?:Ctrl|Alt|Shift|Win)(?:\+[A-Za-z0-9]+)+\b")
VERSION_RE = re.compile(r"\bv?\d+(?:\.\d+)+(?:[-+][A-Za-z0-9.-]+)?\b")
TECHNICAL_RE = re.compile(
    r"\b(?:GDI|GNU|GPL|HDR|HTTP|HTTPS|HUD|JPEG|KB|L1|MB|NTFS|NVIDIA|OBS|PC|PNG|PS|R1|SHA-256|UI)\b"
)
WORD_RE = re.compile(r"[A-Za-z]+")


def _protected_ranges(text: str, protected: list[str]) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    patterns = [PLACEHOLDER_RE, MARKUP_RE, URL_RE, PATH_RE, SHORTCUT_RE, VERSION_RE, TECHNICAL_RE]
    for pattern in patterns:
        ranges.extend((match.start(), match.end()) for match in pattern.finditer(text))
    for token in protected:
        ranges.extend((match.start(), match.end()) for match in re.finditer(re.escape(token), text))
    if not ranges:
        return []
    merged: list[list[int]] = []
    for start, end in sorted(ranges):
        if merged and start < merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [(start, end) for start, end in merged]


def _accent_and_expand(text: str) -> str:
    def expand(match: re.Match[str]) -> str:
        word = match.group(0).translate(ACCENTS)
        return word + "~" * max(1, math.ceil(len(word) * 0.28))

    return WORD_RE.sub(expand, text)


def _rtl_stress(text: str) -> str:
    def stress(match: re.Match[str]) -> str:
        return match.group(0).translate(ACCENTS)[::-1] + "ـ"

    return WORD_RE.sub(stress, text)


def pseudo_text(text: str, locale: str, protected: list[str]) -> str:
    if not text:
        return text
    ranges = _protected_ranges(text, protected)
    pieces: list[str] = []
    cursor = 0
    transform = _accent_and_expand if locale == "en-XA" else _rtl_stress
    for start, end in ranges:
        pieces.append(transform(text[cursor:start]))
        pieces.append(text[start:end])
        cursor = end
    pieces.append(transform(text[cursor:]))
    body = "".join(pieces)
    if locale == "en-XA":
        return f"⟦{body}⟧"
    return f"⟫\u2067{body}\u2069⟪"


def _manifest_tokens(path: Path) -> dict[str, list[str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return {entry["id"]: entry.get("protected_tokens", []) for entry in data["messages"]}


def generate_catalog(source: Path, output: Path, locale: str, tokens: dict[str, list[str]]) -> None:
    tree = ET.parse(source)
    root = tree.getroot()
    root.set("language", locale.replace("-", "_"))
    root.set("sourcelanguage", "en_US")
    for message in root.findall("./context/message"):
        message_id = message.get("id", "")
        protected = tokens.get(message_id, [])
        translation = message.find("translation")
        source_text = message.findtext("source", default="")
        if translation is None:
            translation = ET.SubElement(message, "translation")
        translation.attrib.pop("type", None)
        forms = translation.findall("numerusform")
        if message.get("numerus") == "yes":
            base = [(form.text or source_text) for form in forms] or [source_text, source_text]
            for child in list(translation):
                translation.remove(child)
            translation.text = "\n        "
            count = 2 if locale == "en-XA" else 6
            for index in range(count):
                raw = base[min(index, len(base) - 1)]
                form = ET.SubElement(translation, "numerusform")
                form.text = pseudo_text(raw, locale, protected)
                form.tail = "\n      " if index == count - 1 else "\n        "
        else:
            for child in list(translation):
                translation.remove(child)
            translation.text = pseudo_text(translation.text or source_text, locale, protected)

    ET.indent(tree, space="  ")
    output.parent.mkdir(parents=True, exist_ok=True)
    xml = ET.tostring(root, encoding="unicode", short_empty_elements=False)
    output.write_text(f'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n{xml}\n', encoding="utf-8", newline="\n")


def generate_development_manifest(source: Path, output: Path) -> None:
    data = json.loads(source.read_text(encoding="utf-8"))
    tags = {entry["tag"] for entry in data["locales"]}
    if tags & {"en-XA", "ar-XB"}:
        raise ValueError("production locale registry must not contain pseudo-locales")
    data["$comment"] = (
        "Generated development registry. Public locales come from i18n/locales.json; "
        "pseudo-locales are internal and must never be packaged in production."
    )
    data["locales"].extend([
        {
            "tag": "en-XA",
            "names": {"native": "Accented Pseudo", "english": "Accented Pseudo-locale"},
            "aliases": [], "tier": 0, "state": "internal", "direction": "ltr",
            "fallback": "en-US", "qt_catalog": "gamehq_en_XA",
            "inno_language": None, "inno_message_file": None,
            "completeness_policy": "never_embed",
        },
        {
            "tag": "ar-XB",
            "names": {"native": "RTL Pseudo", "english": "RTL Pseudo-locale"},
            "aliases": [], "tier": 0, "state": "internal", "direction": "rtl",
            "fallback": "en-US", "qt_catalog": "gamehq_ar_XB",
            "inno_language": None, "inno_message_file": None,
            "completeness_policy": "never_embed",
        },
    ])
    data["tiers"]["0"] = "Internal development pseudo-locales; never embedded in production."
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--manifest-output", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    tokens = _manifest_tokens(root / "i18n/extracted/messages.json")
    source = root / "i18n/app/gamehq_en_US.ts"
    generate_catalog(source, args.output_dir / "gamehq_en_XA.ts", "en-XA", tokens)
    generate_catalog(source, args.output_dir / "gamehq_ar_XB.ts", "ar-XB", tokens)
    generate_development_manifest(root / "i18n/locales.json", args.manifest_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
