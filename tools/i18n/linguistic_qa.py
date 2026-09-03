#!/usr/bin/env python3
"""Validate complete per-locale contextual linguistic-review evidence."""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
HASH = re.compile(r"^[0-9a-f]{64}$")
FORBIDDEN = {"conversation", "conversation_contents", "credentials", "prompt", "response_text", "secrets", "telemetry", "user_data"}


class ReviewError(RuntimeError):
    pass


def read_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_bytes().decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReviewError(f"cannot read strict UTF-8 JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise ReviewError(f"expected JSON object: {path}")
    return value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def translation_hash(forms: list[str]) -> str:
    value = json.dumps(forms, ensure_ascii=False, separators=(",", ":"))
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def reject_private(value: object, label: str = "review") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if str(key).casefold() in FORBIDDEN:
                raise ReviewError(f"{label}: forbidden private field {key!r}")
            reject_private(child, f"{label}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_private(child, f"{label}[{index}]")


def catalog_messages(path: Path) -> dict[str, list[str]]:
    try:
        tree = ET.fromstring(path.read_bytes().decode("utf-8"))
    except (OSError, UnicodeDecodeError, ET.ParseError) as error:
        raise ReviewError(f"cannot read catalog {path}: {error}") from error
    result: dict[str, list[str]] = {}
    for message in tree.findall(".//message"):
        message_id = message.get("id")
        translation = message.find("translation")
        if not message_id or translation is None or translation.get("type") == "unfinished":
            raise ReviewError(f"{path}: missing or unfinished catalog message")
        forms = ["".join(item.itertext()) for item in translation.findall("numerusform")]
        if not forms:
            forms = ["".join(translation.itertext())]
        if any(not form.strip() for form in forms):
            raise ReviewError(f"{path}: {message_id} has an empty translation")
        result[message_id] = forms
    return result


def validate_review(root: Path, artifact_path: Path) -> dict[str, object]:
    value = read_json(artifact_path)
    reject_private(value)
    required = {"$schema", "schema_version", "locale", "source_locale", "candidate", "review", "baseline", "reviewed", "coverage", "corrections", "surfaces", "terminology_decisions", "calibration_rules", "unresolved"}
    if set(value) != required or value.get("schema_version") != 1:
        raise ReviewError("review document does not match schema version 1")
    locale = str(value.get("locale"))
    registry = read_json(root / "i18n/locales.json")
    production = {
        str(entry["tag"]) for entry in registry.get("locales", [])
        if isinstance(entry, dict) and entry.get("state") == "enabled"
    }
    if locale not in production or value.get("source_locale") != "en-US" or value.get("candidate") != "0.7.7":
        raise ReviewError("review locale/source/candidate identity is invalid")
    review = value.get("review")
    if not isinstance(review, dict) or review.get("state") != "contextually_reviewed" or review.get("authority") != "p8-3" or review.get("reviewer_kind") != "agent":
        raise ReviewError("review authority or state is invalid")
    try:
        datetime.fromisoformat(str(review.get("recorded_at")).replace("Z", "+00:00"))
    except ValueError as error:
        raise ReviewError("review timestamp is invalid") from error
    if value.get("unresolved") != []:
        raise ReviewError("review contains unresolved linguistic defects")

    extracted = read_json(root / "i18n/extracted/messages.json")
    source = {str(item["id"]): item for item in extracted.get("messages", []) if isinstance(item, dict)}
    state = read_json(root / "i18n/state/translations.json")
    locale_state = state.get("locales", {}).get(locale)
    if not isinstance(locale_state, dict):
        raise ReviewError(f"translation state is missing {locale}")
    catalog_relative = str(locale_state.get("catalog"))
    catalog_path = root / catalog_relative
    catalog = catalog_messages(catalog_path)
    if len(source) != 831 or set(catalog) != set(source):
        raise ReviewError(f"{locale} review requires the exact 831-ID source/catalog set, got {len(source)}/{len(catalog)}")
    reviewed = value.get("reviewed")
    if not isinstance(reviewed, dict) or reviewed.get("path") != catalog_relative or reviewed.get("sha256") != sha256(catalog_path) or reviewed.get("message_count") != 831:
        raise ReviewError("reviewed catalog identity, hash, or count is stale")
    coverage = value.get("coverage")
    if not isinstance(coverage, list) or len(coverage) != 831:
        raise ReviewError("coverage must contain exactly 831 entries")
    coverage_map = {str(entry.get("id")): entry for entry in coverage if isinstance(entry, dict)}
    if set(coverage_map) != set(source):
        raise ReviewError("coverage IDs do not exactly match the canonical source")
    for message_id, entry in coverage_map.items():
        if entry.get("source_hash") != source[message_id].get("source_hash"):
            raise ReviewError(f"{message_id}: stale reviewed source hash")
        if entry.get("translation_hash") != translation_hash(catalog[message_id]):
            raise ReviewError(f"{message_id}: stale reviewed translation hash")
        locations = entry.get("locations")
        if not isinstance(locations, list) or not locations:
            raise ReviewError(f"{message_id}: missing contextual location evidence")
        if entry.get("decision") not in {"accepted", "corrected"}:
            raise ReviewError(f"{message_id}: invalid review decision")
    corrected_ids = {str(entry["id"]) for entry in coverage if entry.get("decision") == "corrected"}
    corrections = value.get("corrections")
    if not isinstance(corrections, list):
        raise ReviewError("corrections must be an array")
    application_corrections = {str(entry.get("id")) for entry in corrections if isinstance(entry, dict) and entry.get("surface") == "application"}
    if application_corrections != corrected_ids:
        raise ReviewError("application correction ledger does not match corrected coverage IDs")
    for entry in corrections:
        if not isinstance(entry, dict) or not all(str(entry.get(field, "")).strip() for field in ("id", "surface", "category", "reason", "terminology_decision")) or entry.get("before") == entry.get("after"):
            raise ReviewError("correction ledger contains an incomplete or no-op delta")
    surfaces = value.get("surfaces")
    expected_surfaces = {"application", "installer", "playnite", "release_notes"}
    if not isinstance(surfaces, list) or {entry.get("surface") for entry in surfaces if isinstance(entry, dict)} != expected_surfaces:
        raise ReviewError("review must cover application, installer, Playnite, and release-note surfaces")
    for surface in surfaces:
        path = root / str(surface.get("path"))
        if not path.is_file() or surface.get("sha256") != sha256(path) or int(surface.get("reviewed_units", 0)) < 1:
            raise ReviewError(f"{surface.get('surface')}: stale or incomplete surface evidence")
    note_state = read_json(root / "assets/release-notes/linguistic-state.json")
    for unit_id, unit in note_state.get("units", {}).items():
        locale_status = unit.get("locales", {}).get(locale) if isinstance(unit, dict) else None
        if locale_status not in {"source_reviewed", "contextually_reviewed", "human_reviewed"}:
            raise ReviewError(f"{locale}: release-note unit {unit_id} remains {locale_status}")
    if locale == "pl-PL" and (catalog["gamehq.navigation.about"] != ["Info"] or catalog["gamehq.navigation.support_gamehq"] != ["Wesprzyj GameHQ"]):
        raise ReviewError("Polish sidebar calibration labels changed")
    if "https://ko-fi.com/underfusion" not in (root / "src/ui/qml/Brand.qml").read_text(encoding="utf-8"):
        raise ReviewError("the exact untranslated Ko-fi URL is missing")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--artifact", default="i18n/quality/reviews/pl-PL.json")
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    artifact = Path(arguments.artifact)
    if not artifact.is_absolute():
        artifact = root / artifact
    try:
        value = validate_review(root, artifact)
    except ReviewError as error:
        print(f"linguistic-review error: {error}", file=sys.stderr)
        return 1
    print(f"linguistic review verified: {value['locale']} 831/831 IDs, no unresolved defects")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
