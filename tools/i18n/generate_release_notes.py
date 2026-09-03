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
    "production_locales", "releases", "localization_launch",
}
# The owner designates the localization-launch release by version. Its final
# date stays null until the owner assigns it, so the date is a release gate and
# never something this tool may invent.
LAUNCH_FIELDS = {"version", "date", "status", "localization_policy", "designated_by"}
LAUNCH_STATUSES = {"designated", "released"}
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


def launch_item_texts(document: dict[str, Any]) -> dict[str, str]:
    return {
        item["id"]: item["text"]
        for section in document["sections"]
        for item in section["items"]
    }


def validate_linguistic_state(
    source_root: Path, launch: dict[str, Any], locales: list[str], english: dict[str, Any]
) -> dict[str, Any]:
    path = source_root / "linguistic-state.json"
    state = read_json(path)
    required = {"$schema", "schema_version", "candidate", "source_integrity", "units"}
    if set(state) != required or state.get("schema_version") != 1:
        fail(f"{path}: invalid linguistic-state document")
    if state.get("candidate") != launch["version"] \
            or state.get("source_integrity") != source_integrity(english):
        fail(f"{path}: stale candidate or source integrity")
    units = state.get("units")
    if not isinstance(units, dict) or not units:
        fail(f"{path}: affected linguistic units are required")
    source_items = launch_item_texts(english)
    allowed = {"source_reviewed", "contextually_reviewed", "human_reviewed", "stale_review_pending"}
    for unit_id, unit in units.items():
        label = f"{path}:{unit_id}"
        if unit_id not in source_items or not isinstance(unit, dict) \
                or set(unit) != {"source_hash", "locales"}:
            fail(f"{label}: unknown unit or invalid state shape")
        expected_hash = hashlib.sha256(source_items[unit_id].encode("utf-8")).hexdigest()
        if unit["source_hash"] != expected_hash:
            fail(f"{label}: stale English source hash")
        states = unit.get("locales")
        if not isinstance(states, dict) or list(states) != locales or set(states.values()) - allowed:
            fail(f"{label}: locale portfolio or review state is invalid")
        if states["en-US"] != "source_reviewed":
            fail(f"{label}: English source must be source-reviewed")
    return state


def validate_document_date(value: Any, expected: str | None, label: str) -> Any:
    """A draft document of a designated launch version keeps its date null until
    the owner assigns one. Released documents always carry a real ISO date."""
    if expected is None:
        if value is not None:
            fail(f"{label}: a draft launch document must keep its date null")
        return None
    if validate_date(value, label) != expected:
        fail(f"{label}: date mismatch")
    return value


def validate_english_document(
    document: dict[str, Any], version: str, expected_date: str | None, label: str
) -> dict[str, Any]:
    if set(document) != ENGLISH_FIELDS:
        fail(f"{label}: English document fields do not match schema version 2")
    if document.get("schema_version") != SCHEMA_VERSION:
        fail(f"{label}: unsupported schema version")
    if document.get("locale") != "en-US":
        fail(f"{label}: locale/tag mismatch")
    if document.get("version") != version:
        fail(f"{label}: version mismatch")
    validate_document_date(document.get("date"), expected_date, label)
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
    validate_document_date(document.get("date"), english["date"], label)
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
    validate_launch(manifest, releases)
    return releases


def validate_launch(
    manifest: dict[str, Any], releases: list[dict[str, Any]]
) -> dict[str, Any] | None:
    launch = manifest.get("localization_launch")
    if launch is None:
        return None
    if not isinstance(launch, dict) or set(launch) != LAUNCH_FIELDS:
        fail("localization_launch fields do not match schema version 2")
    version = launch.get("version")
    version_key(version)
    if launch.get("status") not in LAUNCH_STATUSES:
        fail("localization_launch has an unsupported status")
    if launch.get("localization_policy") != "complete":
        fail("the localization-launch release must require all sixteen locales")
    if not isinstance(launch.get("designated_by"), str) or not launch["designated_by"].strip():
        fail("localization_launch must record who designated it")
    if any(release["version"] == version for release in releases):
        fail(f"localization-launch version {version} is already a released version")
    if version_key(version) <= version_key(releases[0]["version"]):
        fail("the localization-launch release must be newer than every released version")
    date = launch.get("date")
    if launch["status"] == "designated":
        if date is not None:
            fail("a designated localization-launch release must keep its date null "
                 "until the owner assigns it")
    else:
        validate_date(date, "localization_launch")
    return launch


def launch_readiness(
    source_root: Path, manifest: dict[str, Any], locales: list[str]
) -> dict[str, Any]:
    """Report launch readiness without inventing anything.

    Content readiness and release readiness are deliberately separate. Draft
    documents of a designated version are authored, reviewed and validated long
    before the owner assigns the final date; only the date and the owner's
    approval decide release readiness.
    """
    launch = manifest.get("localization_launch")
    if launch is None:
        return {"designated": False,
                "content_blockers": ["no localization-launch release is designated"],
                "release_blockers": ["no localization-launch release is designated"]}
    version_root = source_root / "versions" / launch["version"]
    english_path = version_root / "en-US.json"
    documents: dict[str, str] = {}
    content_blockers: list[str] = []
    english: dict[str, Any] | None = None
    expected_date = launch["date"] if launch["status"] == "released" else None
    if not english_path.is_file():
        content_blockers.append("the authoritative English launch document does not exist")
    else:
        try:
            english = validate_english_document(
                read_json(english_path), launch["version"], expected_date, str(english_path))
            documents["en-US"] = "authored"
        except ReleaseNotesError as error:
            content_blockers.append(f"the English launch document is invalid: {error}")
    linguistic_state: dict[str, Any] | None = None
    if english is not None:
        try:
            linguistic_state = validate_linguistic_state(source_root, launch, locales, english)
        except ReleaseNotesError as error:
            content_blockers.append(f"launch linguistic state is invalid: {error}")
    for locale in locales:
        if locale == "en-US":
            continue
        path = version_root / f"{locale}.json"
        if not path.is_file():
            documents[locale] = "missing"
            continue
        if english is None:
            documents[locale] = "unverified"
            continue
        try:
            document = validate_locale_document(read_json(path), locale, english, str(path))
        except ReleaseNotesError as error:
            documents[locale] = "invalid"
            content_blockers.append(f"{locale}: {error}")
            continue
        documents[locale] = document["mode"]
    missing = sorted(locale for locale, state in documents.items() if state == "missing")
    if missing:
        content_blockers.append(
            f"{len(missing)} locale document(s) are missing: {', '.join(missing)}")
    fallbacks = sorted(locale for locale, state in documents.items() if state == "fallback")
    if fallbacks:
        content_blockers.append(
            "the launch release requires reviewed translations, not English fallback: "
            + ", ".join(fallbacks))
    release_blockers = list(content_blockers)
    if launch["date"] is None:
        release_blockers.append("the owner has not assigned the final release date")
    if launch["status"] != "released":
        release_blockers.append("the owner has not authorized the release")
    return {
        "designated": True,
        "version": launch["version"],
        "date": launch["date"],
        "status": launch["status"],
        "localization_policy": launch["localization_policy"],
        "designated_by": launch["designated_by"],
        "documents": documents,
        "linguistic_state": linguistic_state,
        "content_blockers": content_blockers,
        "release_blockers": release_blockers,
        "content_ready": not content_blockers,
        "release_ready": not release_blockers,
    }


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
    parser.add_argument("--launch-status", action="store_true",
                        help="report designated-launch readiness and gate on content readiness")
    parser.add_argument("--release-ready", action="store_true",
                        help="additionally gate on the owner's final date and approval")
    args = parser.parse_args()
    if args.bootstrap_from:
        bootstrap_legacy(args.bootstrap_from, args.source_root)
    generate_all(args.source_root, args.output_root, args.check)
    print("Release-note generation passed (16 deterministic locale bundles, schema v2)")
    if not (args.launch_status or args.release_ready):
        return 0
    manifest, locales, _ = load_contract(args.source_root)
    readiness = launch_readiness(args.source_root, manifest, locales)
    if not readiness["designated"]:
        print("No localization-launch release is designated.")
        return 1
    print(f"Localization-launch release {readiness['version']} "
          f"({readiness['status']}, designated by {readiness['designated_by']})")
    print(f"  final release date: {readiness['date'] or 'not assigned by the owner'}")
    reviewed = sum(1 for state in readiness["documents"].values()
                   if state in {"authored", "localized"})
    print(f"  reviewed locale documents: {reviewed}/{len(locales)}")
    print(f"  content ready: {'yes' if readiness['content_ready'] else 'no'}")
    for blocker in readiness["content_blockers"]:
        print(f"  content blocker: {blocker}")
    if not args.release_ready:
        return 0 if readiness["content_ready"] else 1
    print(f"  release ready: {'yes' if readiness['release_ready'] else 'no'}")
    for blocker in readiness["release_blockers"]:
        print(f"  release blocker: {blocker}")
    return 0 if readiness["release_ready"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReleaseNotesError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
