#!/usr/bin/env python3
"""Validate versioned release-note sources and build deterministic locale bundles."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
from datetime import date
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
SCHEMA_VERSION = 2
MAX_HISTORY = 12
MAX_SECTIONS = 8
MAX_ITEMS = 20
MAX_TITLE_LENGTH = 80
MAX_ITEM_LENGTH = 1200
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
ID = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
PSEUDO_LOCALES = {"en-XA", "ar-XB"}
MANIFEST_FIELDS = {
    "$schema", "schema_version", "source_locale", "history_limit",
    "production_locales", "releases",
}
RELEASE_FIELDS = {
    "version", "date", "status", "localization_policy", "source_integrity",
    "original_source_integrity", "correction",
}
ENGLISH_FIELDS = {
    "$schema", "schema_version", "version", "locale", "date", "sections",
}
LOCALIZED_FIELDS = ENGLISH_FIELDS | {"mode", "source_integrity"}
FALLBACK_FIELDS = {
    "$schema", "schema_version", "version", "locale", "date", "mode",
    "source_integrity", "fallback_locale",
}


class ReleaseNotesError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ReleaseNotesError(message)


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            fail(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path: Path) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
        text = raw.decode("utf-8", errors="strict")
        value = json.loads(text, object_pairs_hook=unique_object)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"{path}: malformed UTF-8 or JSON: {error}")
    if not isinstance(value, dict):
        fail(f"{path}: root must be an object")
    return value


def json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(json_bytes(value))


def version_key(value: str) -> tuple[int, int, int]:
    if not isinstance(value, str) or not VERSION.fullmatch(value):
        fail(f"invalid release version: {value!r}")
    return tuple(int(part) for part in value.split("."))


def validate_date(value: Any, label: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", value):
        fail(f"{label}: invalid ISO date")
    try:
        date.fromisoformat(value)
    except ValueError:
        fail(f"{label}: invalid ISO date")
    return value


def production_locales(locale_manifest: dict[str, Any]) -> list[str]:
    locales = [
        locale["tag"]
        for locale in locale_manifest.get("locales", [])
        if locale.get("state") == "enabled" and locale.get("tier") == 1
    ]
    if len(locales) != 16 or len(set(locales)) != 16:
        fail("locale registry must expose exactly sixteen unique production locales")
    if "en-US" not in locales or set(locales) & PSEUDO_LOCALES:
        fail("production locales must contain en-US and exclude pseudo-locales")
    return locales


def validate_sections(value: Any, label: str) -> list[dict[str, Any]]:
    if not isinstance(value, list) or not 1 <= len(value) <= MAX_SECTIONS:
        fail(f"{label}: invalid section count")
    section_ids: set[str] = set()
    item_ids: set[str] = set()
    for section_index, section in enumerate(value):
        section_label = f"{label}.sections[{section_index}]"
        if not isinstance(section, dict) or set(section) != {"id", "title", "items"}:
            fail(f"{section_label}: partial or mixed section structure")
        section_id = section["id"]
        title = section["title"]
        items = section["items"]
        if not isinstance(section_id, str) or not ID.fullmatch(section_id):
            fail(f"{section_label}: invalid section ID")
        if section_id in section_ids:
            fail(f"{section_label}: duplicate section ID {section_id}")
        section_ids.add(section_id)
        if not isinstance(title, str) or not title.strip() or len(title) > MAX_TITLE_LENGTH:
            fail(f"{section_label}: invalid title")
        if not isinstance(items, list) or not 1 <= len(items) <= MAX_ITEMS:
            fail(f"{section_label}: invalid item count")
        for item_index, item in enumerate(items):
            item_label = f"{section_label}.items[{item_index}]"
            if not isinstance(item, dict) or set(item) != {"id", "text"}:
                fail(f"{item_label}: partial or mixed item structure")
            item_id = item["id"]
            text = item["text"]
            if not isinstance(item_id, str) or not ID.fullmatch(item_id):
                fail(f"{item_label}: invalid item ID")
            if item_id in item_ids:
                fail(f"{item_label}: duplicate item ID {item_id}")
            item_ids.add(item_id)
            if not isinstance(text, str) or not text.strip() or len(text) > MAX_ITEM_LENGTH:
                fail(f"{item_label}: invalid item text")
    return value


def source_integrity(document: dict[str, Any]) -> str:
    payload = {
        "version": document["version"],
        "date": document["date"],
        "sections": document["sections"],
    }
    canonical = json.dumps(
        payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(canonical).hexdigest()


def validate_english_document(
    document: dict[str, Any], version: str, expected_date: str, label: str
) -> dict[str, Any]:
    if set(document) != ENGLISH_FIELDS:
        fail(f"{label}: English document fields do not match schema version 2")
    if document.get("schema_version") != SCHEMA_VERSION:
        fail(f"{label}: unsupported schema version")
    if document.get("locale") != "en-US":
        fail(f"{label}: locale/tag mismatch")
    if document.get("version") != version:
        fail(f"{label}: version mismatch")
    if validate_date(document.get("date"), label) != expected_date:
        fail(f"{label}: date mismatch")
    validate_sections(document.get("sections"), label)
    return document


def structure_ids(document: dict[str, Any]) -> list[tuple[str, list[str]]]:
    return [
        (section["id"], [item["id"] for item in section["items"]])
        for section in document["sections"]
    ]


def validate_locale_document(
    document: dict[str, Any], locale: str, english: dict[str, Any], label: str
) -> dict[str, Any]:
    if locale == "en-US" or locale in PSEUDO_LOCALES:
        fail(f"{label}: unsupported localized production locale {locale}")
    if document.get("schema_version") != SCHEMA_VERSION:
        fail(f"{label}: unsupported schema version")
    if document.get("locale") != locale:
        fail(f"{label}: locale/tag mismatch")
    if document.get("version") != english["version"]:
        fail(f"{label}: version mismatch")
    if validate_date(document.get("date"), label) != english["date"]:
        fail(f"{label}: date mismatch")
    if document.get("source_integrity") != source_integrity(english):
        fail(f"{label}: stale source-integrity metadata")
    mode = document.get("mode")
    if mode == "fallback":
        if set(document) != FALLBACK_FIELDS or document.get("fallback_locale") != "en-US":
            fail(f"{label}: partial or mixed fallback document structure")
        return document
    if mode != "localized" or set(document) != LOCALIZED_FIELDS:
        fail(f"{label}: partial or mixed localized document structure")
    validate_sections(document.get("sections"), label)
    if structure_ids(document) != structure_ids(english):
        fail(f"{label}: missing, extra, or reordered section/item IDs")
    return document


def validate_manifest(
    manifest: dict[str, Any], expected_locales: list[str]
) -> list[dict[str, Any]]:
    if set(manifest) != MANIFEST_FIELDS or manifest.get("schema_version") != SCHEMA_VERSION:
        fail("release-note manifest does not match schema version 2")
    if manifest.get("source_locale") != "en-US":
        fail("release-note source locale must be en-US")
    if manifest.get("history_limit") != MAX_HISTORY:
        fail(f"release-note history limit must be {MAX_HISTORY}")
    if manifest.get("production_locales") != expected_locales:
        fail("manifest production locales differ from the canonical locale registry")
    if set(manifest["production_locales"]) & PSEUDO_LOCALES:
        fail("pseudo-locales cannot enter production release-note bundles")
    releases = manifest.get("releases")
    if not isinstance(releases, list) or not releases or len(releases) > MAX_HISTORY + 1:
        fail("manifest has an invalid release/history count")
    versions: set[str] = set()
    previous: tuple[int, int, int] | None = None
    for index, release in enumerate(releases):
        label = f"manifest.releases[{index}]"
        if not isinstance(release, dict) or set(release) != RELEASE_FIELDS:
            fail(f"{label}: release metadata fields do not match schema")
        current = version_key(release.get("version"))
        if previous is not None and current >= previous:
            fail(f"{label}: releases are not deterministically newest-first")
        previous = current
        if release["version"] in versions:
            fail(f"{label}: duplicate release version")
        versions.add(release["version"])
        validate_date(release.get("date"), label)
        if release.get("status") != "released":
            fail(f"{label}: unsupported release status")
        if release.get("localization_policy") not in {"fallback-allowed", "complete"}:
            fail(f"{label}: unsupported localization policy")
        for field in ("source_integrity", "original_source_integrity"):
            if not isinstance(release.get(field), str) or not re.fullmatch(
                r"sha256:[0-9a-f]{64}", release[field]
            ):
                fail(f"{label}: invalid {field}")
        correction = release.get("correction")
        changed = release["source_integrity"] != release["original_source_integrity"]
        if changed:
            if not isinstance(correction, dict) or set(correction) != {
                "reason", "approved_by", "corrected_at"
            }:
                fail(f"{label}: released history changed without explicit correction metadata")
            if not all(isinstance(correction[key], str) and correction[key].strip()
                       for key in ("reason", "approved_by")):
                fail(f"{label}: correction metadata is incomplete")
            validate_date(correction["corrected_at"], f"{label}.correction")
        elif correction is not None:
            fail(f"{label}: correction metadata exists without a source correction")
    return releases


def load_contract(source_root: Path) -> tuple[dict[str, Any], list[str], list[tuple[dict[str, Any], dict[str, Any]]]]:
    locale_manifest = read_json(ROOT / "i18n" / "locales.json")
    locales = production_locales(locale_manifest)
    manifest = read_json(source_root / "manifest.json")
    releases = validate_manifest(manifest, locales)
    loaded: list[tuple[dict[str, Any], dict[str, Any]]] = []
    for release in releases:
        version = release["version"]
        english_path = source_root / "versions" / version / "en-US.json"
        english = validate_english_document(
            read_json(english_path), version, release["date"], str(english_path)
        )
        if source_integrity(english) != release["source_integrity"]:
            fail(f"{english_path}: stale manifest source-integrity metadata")
        loaded.append((release, english))
    return manifest, locales, loaded


def strict_validate_locales(
    source_root: Path,
    locales: list[str],
    releases: list[tuple[dict[str, Any], dict[str, Any]]],
) -> None:
    for release, english in releases:
        version_root = source_root / "versions" / release["version"]
        for locale in locales:
            if locale == "en-US":
                continue
            path = version_root / f"{locale}.json"
            if not path.is_file():
                if release["localization_policy"] == "complete":
                    fail(f"{path}: complete releases require all sixteen locale documents")
                continue
            document = validate_locale_document(read_json(path), locale, english, str(path))
            if release["localization_policy"] == "complete" and document["mode"] != "localized":
                fail(f"{path}: complete releases cannot use English fallback")


def resolve_document(
    source_root: Path,
    locale: str,
    release: dict[str, Any],
    english: dict[str, Any],
) -> tuple[dict[str, Any], str, str | None]:
    if locale == "en-US":
        return english, "en-US", None
    path = source_root / "versions" / release["version"] / f"{locale}.json"
    try:
        if not path.is_file():
            fail("locale document is missing")
        localized = validate_locale_document(read_json(path), locale, english, str(path))
        if localized["mode"] == "fallback":
            return english, "en-US", "declared-en-US-fallback"
        return localized, locale, None
    except ReleaseNotesError as error:
        if release["localization_policy"] == "complete":
            raise
        return english, "en-US", str(error)


def legacy_release(document: dict[str, Any]) -> dict[str, Any]:
    return {
        "version": document["version"],
        "date": document["date"],
        "sections": [
            {
                "title": section["title"],
                "items": [item["text"] for item in section["items"]],
            }
            for section in document["sections"]
        ],
    }


def build_bundle(
    source_root: Path,
    locale: str,
    locales: list[str],
    releases: list[tuple[dict[str, Any], dict[str, Any]]],
) -> dict[str, Any]:
    if locale not in locales or locale in PSEUDO_LOCALES:
        fail(f"unsupported production locale: {locale}")
    resolved: list[dict[str, Any]] = []
    metadata: list[dict[str, Any]] = []
    for release, english in releases:
        document, resolved_locale, fallback_reason = resolve_document(
            source_root, locale, release, english
        )
        resolved.append(legacy_release(document))
        metadata.append({
            "version": release["version"],
            "source_integrity": release["source_integrity"],
            "resolved_locale": resolved_locale,
            "fallback": resolved_locale != locale,
            "fallback_reason": fallback_reason,
        })
    current = copy.deepcopy(resolved[0])
    current["history"] = resolved[1:]
    current["_meta"] = {
        "schema_version": SCHEMA_VERSION,
        "requested_locale": locale,
        "source_locale": "en-US",
        "documents": metadata,
    }
    return current


def generate_all(source_root: Path, output_root: Path, check: bool = False) -> None:
    _, locales, releases = load_contract(source_root)
    strict_validate_locales(source_root, locales, releases)
    bundle_records: list[dict[str, Any]] = []
    for locale in locales:
        filename = f"release-notes.{locale}.json"
        path = output_root / filename
        expected = json_bytes(build_bundle(source_root, locale, locales, releases))
        bundle_records.append({
            "locale": locale,
            "filename": filename,
            "size": len(expected),
            "sha256": hashlib.sha256(expected).hexdigest(),
        })
        if check:
            if not path.is_file() or path.read_bytes() != expected:
                fail(f"{path}: generated release-note bundle is stale")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(expected)
    index = {
        "schema_version": SCHEMA_VERSION,
        "source_locale": "en-US",
        "current_version": releases[0][0]["version"],
        "history_limit": MAX_HISTORY,
        "documents": [
            {
                "version": release["version"],
                "date": release["date"],
                "source_integrity": release["source_integrity"],
            }
            for release, _ in releases
        ],
        "bundles": bundle_records,
    }
    index_path = output_root / "release-notes.index.json"
    expected_index = json_bytes(index)
    if check:
        if not index_path.is_file() or index_path.read_bytes() != expected_index:
            fail(f"{index_path}: generated release-note index is stale")
    else:
        index_path.parent.mkdir(parents=True, exist_ok=True)
        index_path.write_bytes(expected_index)


def slug(value: str) -> str:
    result = re.sub(r"[^a-z0-9]+", "-", value.casefold()).strip("-")
    return result or "section"


def bootstrap_legacy(legacy_path: Path, source_root: Path) -> None:
    manifest_path = source_root / "manifest.json"
    if manifest_path.exists():
        fail(f"{manifest_path}: refusing to overwrite an existing source contract")
    legacy = read_json(legacy_path)
    raw_releases = [{key: legacy[key] for key in ("version", "date", "sections")}]
    raw_releases.extend(legacy.get("history", []))
    locale_manifest = read_json(ROOT / "i18n" / "locales.json")
    locales = production_locales(locale_manifest)
    manifest_releases: list[dict[str, Any]] = []
    for raw in raw_releases:
        version = raw.get("version")
        version_key(version)
        release_date = validate_date(raw.get("date"), f"legacy {version}")
        section_counts: dict[str, int] = {}
        sections: list[dict[str, Any]] = []
        for section in raw.get("sections", []):
            base = slug(section.get("title", ""))
            section_counts[base] = section_counts.get(base, 0) + 1
            section_id = base if section_counts[base] == 1 else f"{base}-{section_counts[base]}"
            sections.append({
                "id": section_id,
                "title": section["title"],
                "items": [
                    {"id": f"{section_id}-{index:02d}", "text": text}
                    for index, text in enumerate(section["items"], start=1)
                ],
            })
        english = {
            "$schema": "../../../../i18n/schema/release-note-document.schema.json",
            "schema_version": SCHEMA_VERSION,
            "version": version,
            "locale": "en-US",
            "date": release_date,
            "sections": sections,
        }
        validate_english_document(english, version, release_date, f"bootstrap {version}")
        integrity = source_integrity(english)
        write_json(source_root / "versions" / version / "en-US.json", english)
        for locale in locales:
            if locale == "en-US":
                continue
            fallback = {
                "$schema": "../../../../i18n/schema/release-note-document.schema.json",
                "schema_version": SCHEMA_VERSION,
                "version": version,
                "locale": locale,
                "date": release_date,
                "mode": "fallback",
                "source_integrity": integrity,
                "fallback_locale": "en-US",
            }
            write_json(source_root / "versions" / version / f"{locale}.json", fallback)
        manifest_releases.append({
            "version": version,
            "date": release_date,
            "status": "released",
            "localization_policy": "fallback-allowed",
            "source_integrity": integrity,
            "original_source_integrity": integrity,
            "correction": None,
        })
    manifest = {
        "$schema": "../../i18n/schema/release-notes-manifest.schema.json",
        "schema_version": SCHEMA_VERSION,
        "source_locale": "en-US",
        "history_limit": MAX_HISTORY,
        "production_locales": locales,
        "releases": manifest_releases,
    }
    write_json(manifest_path, manifest)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT / "assets" / "release-notes")
    parser.add_argument("--output-root", type=Path,
                        default=ROOT / "assets" / "release-notes" / "generated")
    parser.add_argument("--bootstrap-from", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.bootstrap_from:
        bootstrap_legacy(args.bootstrap_from, args.source_root)
    generate_all(args.source_root, args.output_root, args.check)
    print("Release-note generation passed (16 deterministic locale bundles, schema v2)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReleaseNotesError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
