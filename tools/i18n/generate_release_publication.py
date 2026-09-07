#!/usr/bin/env python3
"""Render deterministic publication documents from the versioned release-note source.

Publication artifacts are outputs only. They are never an authoring source, and
they are never an update-authorization, signature, version-selection, download,
or installation trust input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

from generate_release_notes import (
    PSEUDO_LOCALES,
    ROOT,
    ReleaseNotesError,
    fail,
    json_bytes,
    load_contract,
    read_json,
    resolve_document,
    strict_validate_locales,
)


PUBLICATION_SCHEMA_VERSION = 1
DEFAULT_OUTPUT_ROOT = ROOT / "assets" / "release-notes" / "publication"
BODY_FILENAME = "RELEASE_BODY.md"
METADATA_FILENAME = "publication-metadata.json"
LOCALE_INDEX_MARKER = "<!-- gamehq:locale-index -->"
REPOSITORY_URL = "https://github.com/underfusion/GameHQ"
LOCALIZED_NOTES_URL = (
    REPOSITORY_URL + "/tree/v{version}/assets/release-notes/publication/{version}"
)
LOCALE_DOCUMENT_LINK = "](release-notes."
CONTROL_CHARACTERS = set(range(0x00, 0x20)) | {0x7F}


def single_line(value: str, label: str) -> str:
    """Reject anything that cannot survive a single Markdown line losslessly."""
    if any(ord(character) in CONTROL_CHARACTERS for character in value):
        fail(f"{label}: control characters cannot be published as Markdown")
    if value != value.strip():
        fail(f"{label}: leading or trailing whitespace cannot be published as Markdown")
    return value


def validate_publication_locales(locales: list[str], production: list[str]) -> list[str]:
    """Publication output covers the production locales exactly once each."""
    if len(locales) != len(set(locales)):
        fail("publication locales must be unique")
    for locale in locales:
        if locale in PSEUDO_LOCALES:
            fail(f"pseudo-locale cannot be published: {locale}")
        if locale not in production:
            fail(f"unsupported publication locale: {locale}")
    if set(locales) != set(production):
        fail("publication must cover every production locale exactly once")
    return locales


def locale_names(locales: list[str]) -> dict[str, dict[str, str]]:
    registry = read_json(ROOT / "i18n" / "locales.json")
    names = {
        entry["tag"]: entry["names"]
        for entry in registry.get("locales", [])
        if isinstance(entry, dict) and isinstance(entry.get("names"), dict)
    }
    missing = [locale for locale in locales if locale not in names]
    if missing:
        fail(f"locale registry has no display names for: {', '.join(missing)}")
    return {locale: names[locale] for locale in locales}


def render_document(
    document: dict[str, Any], requested_locale: str, effective_locale: str,
    names: dict[str, dict[str, str]],
) -> str:
    """Render one publication document. The body comes from exactly one source
    document, so a document is never part translated and part English."""
    label = f"publication {requested_locale} {document['version']}"
    lines = [f"# GameHQ {document['version']} ({document['date']})", ""]
    if effective_locale != requested_locale:
        english_name = names[requested_locale]["english"]
        lines.append(
            f"> This document is the original **English ({effective_locale})** release note. "
            f"No reviewed {english_name} translation exists for this version, "
            f"so the complete English text is published unchanged."
        )
        lines.append("")
    for section in document["sections"]:
        lines.append(f"## {single_line(section['title'], label)}")
        lines.append("")
        for item in section["items"]:
            lines.append(f"- {single_line(item['text'], label)}")
        lines.append("")
    return "\n".join(lines).rstrip("\n") + "\n"


def parse_document(text: str, label: str) -> dict[str, Any]:
    """Read a rendered publication document back into its structured meaning."""
    version = ""
    release_date = ""
    sections: list[dict[str, Any]] = []
    for line in text.split("\n"):
        if line.startswith("# GameHQ "):
            heading = line[len("# GameHQ "):]
            if not heading.endswith(")") or " (" not in heading:
                fail(f"{label}: unreadable publication heading")
            version, _, tail = heading.partition(" (")
            release_date = tail[:-1]
        elif line.startswith("## "):
            sections.append({"title": line[3:], "items": []})
        elif line.startswith("- "):
            if not sections:
                fail(f"{label}: publication item outside a section")
            sections[-1]["items"].append(line[2:])
        elif line and not line.startswith("> "):
            fail(f"{label}: unexpected publication line: {line[:40]}")
    if not version or not release_date or not sections:
        fail(f"{label}: publication document lost its version, date, or sections")
    return {"version": version, "date": release_date, "sections": sections}


def structured_meaning(document: dict[str, Any]) -> dict[str, Any]:
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


def assert_equivalent(rendered: str, document: dict[str, Any], label: str) -> None:
    if parse_document(rendered, label) != structured_meaning(document):
        fail(f"{label}: rendered Markdown is not equivalent to the structured source")


def build_localized_documents(
    source_root: Path, locales: list[str], release: dict[str, Any],
    english: dict[str, Any], names: dict[str, dict[str, str]],
) -> tuple[list[dict[str, Any]], dict[str, bytes]]:
    """Render one repository document per production locale.

    These documents are internal publication inputs: the application, the
    updater and the repository consume them. They are never uploaded as
    individual GitHub Release assets.
    """
    assets: list[dict[str, Any]] = []
    files: dict[str, bytes] = {}
    for locale in locales:
        document, effective_locale, fallback_reason = resolve_document(
            source_root, locale, release, english
        )
        rendered = render_document(document, locale, effective_locale, names)
        label = f"{locale} {release['version']}"
        assert_equivalent(rendered, document, label)
        payload = rendered.encode("utf-8")
        filename = f"release-notes.{locale}.md"
        if filename in files:
            fail(f"{label}: duplicate publication filename {filename}")
        files[filename] = payload
        assets.append({
            "canonical_locale": locale,
            "requested_locale": locale,
            "effective_locale": effective_locale,
            "fallback": effective_locale != locale,
            "fallback_state": "authored" if effective_locale == locale
                              else "whole-document-fallback",
            "fallback_reason": fallback_reason,
            "language_english_name": names[locale]["english"],
            "language_native_name": names[locale]["native"],
            "version": release["version"],
            "date": release["date"],
            "filename": filename,
            "size": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "source_integrity": release["source_integrity"],
            "structure": [
                {
                    "section_id": section["id"],
                    "item_ids": [item["id"] for item in section["items"]],
                }
                for section in document["sections"]
            ],
        })
    return assets, files


def render_body(
    english: dict[str, Any], assets: list[dict[str, Any]],
    names: dict[str, dict[str, str]], version: str,
) -> str:
    """A concise English release body: the default English notes plus one link
    to the localized documents kept in the repository.

    The body never links a per-locale file, because those files are not GitHub
    Release assets. Release assets stay limited to installable packages and
    verification metadata.
    """
    lines = render_document(english, "en-US", "en-US", names).rstrip("\n").split("\n")
    url = LOCALIZED_NOTES_URL.format(version=version)
    lines.extend(["", LOCALE_INDEX_MARKER, "", "## Release notes in other languages", ""])
    lines.append(
        "These release notes are published in English by default. "
        f"[Release notes in {len(assets)} languages]({url}) are kept in the "
        "repository; they are not attached to this release as separate "
        "downloads."
    )
    lines.extend(["", "| Language | Locale | Content |", "| --- | --- | --- |"])
    for asset in assets:
        if asset["effective_locale"] == "en-US" and asset["requested_locale"] == "en-US":
            content = "English source"
        elif asset["fallback"]:
            content = "English fallback (no reviewed translation)"
        else:
            content = "Reviewed translation"
        lines.append(
            f"| {asset['language_native_name']} | `{asset['canonical_locale']}` | "
            f"{content} |"
        )
    return "\n".join(lines).rstrip("\n") + "\n"


def body_release_notes(text: str, label: str) -> dict[str, Any]:
    """The release body's own release-note content, without the locale index."""
    head, marker, _ = text.partition(LOCALE_INDEX_MARKER)
    if not marker:
        fail(f"{label}: release body has no locale index marker")
    return parse_document(head, label)


def build_release(
    source_root: Path, locales: list[str], release: dict[str, Any],
    english: dict[str, Any], names: dict[str, dict[str, str]],
) -> dict[str, bytes]:
    version = release["version"]
    assets, files = build_localized_documents(
        source_root, locales, release, english, names
    )
    body = render_body(english, assets, names, version)
    label = f"release body {version}"
    if body_release_notes(body, label) != structured_meaning(english):
        fail(f"{label}: release body is not equivalent to the structured English source")
    localized_notes_url = LOCALIZED_NOTES_URL.format(version=version)
    if f"]({localized_notes_url})" not in body:
        fail(f"{label}: release body does not link the localized notes in the repository")
    if LOCALE_DOCUMENT_LINK in body:
        fail(f"{label}: per-locale documents are not GitHub Release assets and "
             "must not be linked as release downloads")
    body_payload = body.encode("utf-8")
    files[BODY_FILENAME] = body_payload
    metadata = {
        "schema_version": PUBLICATION_SCHEMA_VERSION,
        "kind": "release-publication",
        "version": release["version"],
        "date": release["date"],
        "source_locale": "en-US",
        "default_locale": "en-US",
        "localization_policy": release["localization_policy"],
        "release_source_integrity": release["source_integrity"],
        "release_body": {
            "filename": BODY_FILENAME,
            "size": len(body_payload),
            "sha256": hashlib.sha256(body_payload).hexdigest(),
        },
        "distribution": {
            "localized_documents": "repository",
            "github_release_assets": False,
            "localized_documents_url": localized_notes_url,
        },
        "localized_documents": assets,
    }
    files[METADATA_FILENAME] = json_bytes(metadata)
    return files


def build_publication(
    source_root: Path, only_version: str | None = None
) -> dict[str, bytes]:
    manifest, production, releases = load_contract(source_root)
    strict_validate_locales(source_root, production, releases)
    locales = validate_publication_locales(manifest["production_locales"], production)
    names = locale_names(locales)
    selected = [pair for pair in releases
                if only_version is None or pair[0]["version"] == only_version]
    if not selected:
        fail(f"no released version matches {only_version!r}")
    files: dict[str, bytes] = {}
    for release, english in selected:
        for name, payload in build_release(
            source_root, locales, release, english, names
        ).items():
            files[f"{release['version']}/{name}"] = payload
    return files


def existing_files(output_root: Path) -> set[str]:
    if not output_root.is_dir():
        return set()
    return {
        path.relative_to(output_root).as_posix()
        for path in sorted(output_root.rglob("*")) if path.is_file()
    }


def publish(
    source_root: Path, output_root: Path, only_version: str | None, check: bool
) -> int:
    files = build_publication(source_root, only_version)
    present = existing_files(output_root)
    scope = {name for name in present
             if only_version is None or name.startswith(f"{only_version}/")}
    if check:
        for name in sorted(set(files) | scope):
            path = output_root / name
            if name not in files:
                fail(f"{path}: unexpected publication asset")
            if name not in scope or not path.is_file():
                fail(f"{path}: missing publication asset")
            if path.read_bytes() != files[name]:
                fail(f"{path}: stale publication asset")
    else:
        for name in sorted(scope - set(files)):
            (output_root / name).unlink()
        for name, payload in sorted(files.items()):
            path = output_root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
    return len(files)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path,
                        default=ROOT / "assets" / "release-notes")
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--version")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    written = publish(args.source_root, args.output_root, args.version, args.check)
    verb = "verified" if args.check else "written"
    print(f"Release publication {verb} ({written} deterministic artifacts)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReleaseNotesError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
