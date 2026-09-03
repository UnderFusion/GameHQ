#!/usr/bin/env python3
"""Deterministic, explicit Qt ID catalog extraction and synchronization."""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


SOURCE_EXTENSIONS = {".cpp", ".cxx", ".cc", ".h", ".hpp", ".qml", ".js"}
ID_PATTERN = re.compile(r"^gamehq(?:\.[a-z0-9][a-z0-9_-]*)+$")
ID_CALL = re.compile(
    r"\b(qsTrId|qtTrId|QT_TRID_NOOP|QT_TRID_N_NOOP)\s*\(\s*"
    r"(\"(?:\\.|[^\"\\])*\")"
)
SOURCE_COMMENT = re.compile(r'^\s*//%\s*("(?:\\.|[^"\\])*")\s*$')
PLACEHOLDER = re.compile(r"%(?:L?\d+|n)")
MARKUP = re.compile(r"<\s*(/?)\s*([A-Za-z][\w:-]*)(?:\s[^>]*)?(/?)\s*>")
PROTECTED_LITERALS = (
    "GameHQUpdater.exe",
    "GameHQ.exe",
    "GNU GPL v3",
    "GameHQ",
    "underfusion",
)
PROTECTED_PATTERNS = (
    re.compile(r"https?://[^\s<>\"']+"),
    re.compile(r"\b[A-Za-z0-9_.-]+\.exe\b", re.IGNORECASE),
    re.compile(r"(?<![\w-])--[a-z0-9][a-z0-9-]*\b"),
    re.compile(r"\b(?:HKCU|HKLM|HKEY_CURRENT_USER|HKEY_LOCAL_MACHINE)\\[^\s,;]+", re.IGNORECASE),
    re.compile(r"\b(?:[a-z][a-z0-9_-]*\.){2,}[a-z0-9_-]+\b"),
)


class SyncError(RuntimeError):
    pass


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def decode_quoted(value: str, location: str) -> str:
    try:
        decoded = json.loads(value)
    except json.JSONDecodeError as error:
        raise SyncError(f"{location}: source comment is not a valid quoted string: {error}")
    if not isinstance(decoded, str) or not decoded:
        raise SyncError(f"{location}: source comment must contain non-empty English text")
    return decoded


def source_files(source_dir: Path) -> list[Path]:
    return sorted(
        path for path in source_dir.rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_EXTENSIONS
    )


def scan_source_ids(root: Path, source_dir: Path) -> dict[str, dict[str, object]]:
    messages: dict[str, dict[str, object]] = {}
    for path in source_files(source_dir):
        text = path.read_text(encoding="utf-8")
        lines = text.splitlines()
        newlines = [index for index, char in enumerate(text) if char == "\n"]
        for match in ID_CALL.finditer(text):
            line_number = bisect.bisect_right(newlines, match.start()) + 1
            call_line_index = line_number - 1
            comment_index = call_line_index - 1
            while comment_index >= 0 and not lines[comment_index].strip():
                comment_index -= 1
            relative = path.relative_to(root).as_posix()
            location = f"{relative}:{line_number}"
            if comment_index < 0:
                raise SyncError(
                    f"{location}: missing source comment //% \"English source\""
                )
            source_match = SOURCE_COMMENT.match(lines[comment_index])
            if not source_match:
                raise SyncError(
                    f"{location}: missing source comment //% \"English source\""
                )

            message_id = decode_quoted(match.group(2), location)
            if not ID_PATTERN.fullmatch(message_id):
                raise SyncError(f"{location}: invalid message ID '{message_id}'")
            source = decode_quoted(source_match.group(1), location)
            item = messages.setdefault(message_id, {"source": source, "locations": []})
            if item["source"] != source:
                raise SyncError(
                    f"{location}: duplicate ID '{message_id}' has conflicting English source"
                )
            item["locations"].append({"file": relative, "line": line_number})
    if not messages:
        raise SyncError(f"no ID-based messages found below {source_dir}")
    return messages


def run_lupdate(lupdate: Path, source_dir: Path) -> dict[str, dict[str, object]]:
    with tempfile.TemporaryDirectory(prefix="gamehq-i18n-") as temp_dir:
        extracted = Path(temp_dir) / "extracted.ts"
        command = [
            str(lupdate), str(source_dir), "-recursive", "-locations", "absolute",
            "-source-language", "en_US", "-target-language", "en_US",
            "-ts", str(extracted),
        ]
        process = subprocess.run(command, capture_output=True, text=True, errors="replace")
        if process.returncode != 0:
            detail = (process.stderr or process.stdout).strip()
            raise SyncError(f"lupdate failed ({process.returncode}): {detail}")
        try:
            root = ET.parse(extracted).getroot()
        except (OSError, ET.ParseError) as error:
            raise SyncError(f"lupdate produced invalid TS XML: {error}")

    messages: dict[str, dict[str, object]] = {}
    for context in root.findall("context"):
        context_name = context.findtext("name", default="")
        for message in context.findall("message"):
            message_id = message.get("id", "")
            if not message_id:
                continue
            source = message.findtext("source", default="")
            item = messages.setdefault(
                message_id,
                {"source": source, "plural": False, "contexts": set()},
            )
            if item["source"] != source:
                raise SyncError(
                    f"lupdate returned conflicting sources for duplicate ID '{message_id}'"
                )
            item["plural"] = bool(item["plural"] or message.get("numerus") == "yes")
            item["contexts"].add(context_name)
    return messages


def markup_signature(source: str) -> list[str]:
    signature: list[str] = []
    for match in MARKUP.finditer(source):
        name = match.group(2).lower()
        if match.group(1):
            signature.append(f"</{name}>")
        elif match.group(3):
            signature.append(f"<{name}/>")
        else:
            signature.append(f"<{name}>")
    return signature


def protected_tokens(source: str) -> list[str]:
    tokens: list[str] = []
    for literal in PROTECTED_LITERALS:
        bounded = re.compile(rf"(?<![\w]){re.escape(literal)}(?![\w])")
        tokens.extend(literal for _ in bounded.finditer(source))
    for pattern in PROTECTED_PATTERNS:
        tokens.extend(match.group(0).rstrip(".,;:!?)") for match in pattern.finditer(source))
    return sorted(tokens)


def message_domain(message_id: str) -> str:
    if message_id.startswith("gamehq.installer."):
        return "installer"
    if message_id.startswith("gamehq.release_notes.content."):
        return "release_notes"
    return "application"


def message_source_hash(source: str, context: str) -> str:
    return hashlib.sha256(f"{context}\0{source}".encode("utf-8")).hexdigest()


def build_manifest(
    scanned: dict[str, dict[str, object]], extracted: dict[str, dict[str, object]]
) -> dict[str, object]:
    scanned_ids = set(scanned)
    extracted_ids = set(extracted)
    if scanned_ids != extracted_ids:
        missing = sorted(scanned_ids - extracted_ids)
        unexpected = sorted(extracted_ids - scanned_ids)
        raise SyncError(
            f"lupdate/source scan disagree; missing={missing}, unexpected={unexpected}"
        )

    messages: list[dict[str, object]] = []
    for message_id in sorted(scanned):
        source = str(scanned[message_id]["source"])
        qt_message = extracted[message_id]
        if qt_message["source"] != source:
            raise SyncError(
                f"ID '{message_id}' source differs between //% comment and lupdate output"
            )
        contexts = sorted(str(value) for value in qt_message["contexts"] if value)
        context = contexts[0] if contexts else "ID"
        locations = sorted(
            scanned[message_id]["locations"],
            key=lambda value: (str(value["file"]), int(value["line"])),
        )
        messages.append({
            "id": message_id,
            "source": source,
            "context": context,
            "domain": message_domain(message_id),
            "locations": locations,
            "plural": bool(qt_message["plural"]),
            "placeholders": sorted(set(PLACEHOLDER.findall(source))),
            "markup_signature": markup_signature(source),
            "protected_tokens": protected_tokens(source),
            "source_hash": message_source_hash(source, context),
        })
    manifest: dict[str, object] = {
        "$schema": "../schema/extracted-messages.schema.json",
        "schema_version": 1,
        "source_language": "en-US",
        "messages": messages,
    }
    validate_manifest(manifest)
    return manifest


def validate_manifest(manifest: dict[str, object]) -> None:
    if set(manifest) != {"$schema", "schema_version", "source_language", "messages"}:
        raise SyncError("generated extraction manifest has unexpected top-level fields")
    messages = manifest.get("messages")
    if not isinstance(messages, list):
        raise SyncError("generated extraction manifest messages must be an array")
    previous = ""
    required = {
        "id", "source", "context", "domain", "locations", "plural",
        "placeholders", "markup_signature", "protected_tokens", "source_hash",
    }
    for message in messages:
        if not isinstance(message, dict) or set(message) != required:
            raise SyncError("generated extraction message does not match its schema")
        message_id = message["id"]
        if not isinstance(message_id, str) or not ID_PATTERN.fullmatch(message_id):
            raise SyncError("generated extraction manifest contains an invalid ID")
        if message_id <= previous:
            raise SyncError("generated extraction messages are not uniquely sorted")
        previous = message_id
        if message["domain"] not in {"application", "installer", "release_notes"}:
            raise SyncError(f"generated domain is invalid for '{message_id}'")
        if not re.fullmatch(r"[0-9a-f]{64}", str(message["source_hash"])):
            raise SyncError(f"generated source hash is invalid for '{message_id}'")


def parse_catalog(path: Path) -> ET.Element:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        raise SyncError(f"cannot parse catalog {path}: {error}")
    if root.tag != "TS":
        raise SyncError(f"catalog {path} does not have a TS root")
    return root


def catalog_messages(root: ET.Element, path: Path) -> dict[str, ET.Element]:
    result: dict[str, ET.Element] = {}
    for message in root.findall("./context/message"):
        message_id = message.get("id", "")
        if not message_id:
            continue
        if message_id in result:
            raise SyncError(f"catalog {path} contains duplicate ID '{message_id}'")
        result[message_id] = message
    return result


def ensure_child(parent: ET.Element, tag: str) -> ET.Element:
    child = parent.find(tag)
    if child is None:
        child = ET.SubElement(parent, tag)
    return child


def context_for(root: ET.Element, name: str) -> ET.Element:
    for context in root.findall("context"):
        if context.findtext("name", default="") == name:
            return context
    context = ET.SubElement(root, "context")
    ET.SubElement(context, "name").text = name
    return context


def replace_locations(message: ET.Element, locations: list[dict[str, object]]) -> None:
    for location in list(message.findall("location")):
        message.remove(location)
    for index, location in enumerate(locations):
        element = ET.Element("location", {
            "filename": str(location["file"]),
            "line": str(location["line"]),
        })
        message.insert(index, element)


def set_source_translation(translation: ET.Element, source: str, plural: bool) -> None:
    translation.attrib.pop("type", None)
    for child in list(translation):
        translation.remove(child)
    if plural:
        translation.text = None
        ET.SubElement(translation, "numerusform").text = source
        ET.SubElement(translation, "numerusform").text = source
    else:
        translation.text = source


def has_complete_source_plural(translation: ET.Element, source: str) -> bool:
    forms = translation.findall("numerusform")
    source_placeholders = sorted(set(PLACEHOLDER.findall(source)))
    return len(forms) >= 2 and all(
        (text := "".join(form.itertext()).strip())
        and sorted(set(PLACEHOLDER.findall(text))) == source_placeholders
        and markup_signature(text) == markup_signature(source)
        for form in forms
    )


def sort_catalog(root: ET.Element) -> None:
    contexts = list(root.findall("context"))
    for context in contexts:
        messages = list(context.findall("message"))
        for message in messages:
            context.remove(message)
        messages.sort(key=lambda message: (
            0 if message.get("id") else 1,
            message.get("id", ""),
            message.findtext("source", default=""),
        ))
        context.extend(messages)
        root.remove(context)
    contexts.sort(key=lambda context: context.findtext("name", default=""))
    root.extend(contexts)


def catalog_bytes(root: ET.Element) -> bytes:
    ET.indent(root, space="  ")
    xml = ET.tostring(root, encoding="unicode", short_empty_elements=False)
    return (f'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n{xml}\n').encode("utf-8")


def locale_tag(root: ET.Element, path: Path) -> str:
    value = root.get("language", "").replace("_", "-")
    if not value:
        raise SyncError(f"catalog {path} has no TS language attribute")
    return value


def translation_text(translation: ET.Element | None) -> str:
    return "" if translation is None else "".join(translation.itertext()).strip()


def synchronize_catalog(
    path: Path, root: ET.Element, manifest: dict[str, object], source_locale: str
) -> tuple[bytes, dict[str, list[str]], dict[str, int]]:
    active = {message["id"]: message for message in manifest["messages"]}
    existing = catalog_messages(root, path)
    is_source = locale_tag(root, path).casefold() == source_locale.casefold()
    changes = {"added": 0, "changed": 0, "obsolete": 0, "unchanged": 0}

    for message_id, extracted in active.items():
        message = existing.get(message_id)
        if message is None:
            context_name = extracted["context"] if extracted["context"] != "ID" else "GameHQ"
            message = ET.SubElement(context_for(root, str(context_name)), "message", {"id": message_id})
            if extracted["plural"]:
                message.set("numerus", "yes")
            ET.SubElement(message, "source").text = str(extracted["source"])
            translation = ET.SubElement(message, "translation")
            if is_source:
                set_source_translation(translation, str(extracted["source"]), bool(extracted["plural"]))
            else:
                translation.set("type", "unfinished")
            existing[message_id] = message
            changes["added"] += 1
        else:
            current_source = message.findtext("source", default="")
            source = str(extracted["source"])
            translation = ensure_child(message, "translation")
            source_changed = current_source != source
            if source_changed:
                source_element = ensure_child(message, "source")
                old_source = message.find("oldsource")
                if old_source is None:
                    old_source = ET.Element("oldsource")
                    message.insert(list(message).index(source_element) + 1, old_source)
                old_source.text = current_source
                source_element.text = source
                if not is_source:
                    translation.set("type", "unfinished")
                changes["changed"] += 1
            else:
                changes["unchanged"] += 1
            if extracted["plural"]:
                message.set("numerus", "yes")
            else:
                message.attrib.pop("numerus", None)
            if is_source:
                old_source = message.find("oldsource")
                if old_source is not None:
                    message.remove(old_source)
                if (not extracted["plural"] or source_changed
                        or not has_complete_source_plural(translation, source)):
                    set_source_translation(translation, source, bool(extracted["plural"]))
            elif translation.get("type") in {"vanished", "obsolete"}:
                if translation_text(translation):
                    translation.attrib.pop("type", None)
                else:
                    translation.set("type", "unfinished")
        replace_locations(message, list(extracted["locations"]))

    obsolete = sorted(set(existing) - set(active))
    for message_id in obsolete:
        translation = ensure_child(existing[message_id], "translation")
        translation.set("type", "vanished")
    changes["obsolete"] = len(obsolete)

    sort_catalog(root)
    refreshed = catalog_messages(root, path)
    status = {"missing": [], "stale": [], "obsolete": obsolete, "unchanged": []}
    for message_id in sorted(active):
        message = refreshed[message_id]
        translation = message.find("translation")
        if is_source:
            status["unchanged"].append(message_id)
        elif translation is not None and translation.get("type") == "unfinished" \
                and message.find("oldsource") is not None:
            status["stale"].append(message_id)
        elif translation is None or translation.get("type") == "unfinished" or not translation_text(translation):
            status["missing"].append(message_id)
        else:
            status["unchanged"].append(message_id)
    return catalog_bytes(root), status, changes


def validate_report(report: dict[str, object]) -> None:
    required = {"$schema", "schema_version", "source_manifest", "locales"}
    if set(report) != required or not isinstance(report["locales"], dict):
        raise SyncError("generated catalog report does not match its schema")
    for locale, status in report["locales"].items():
        if not isinstance(locale, str) or not isinstance(status, dict):
            raise SyncError("generated catalog report contains an invalid locale")
        if set(status) != {"catalog", "missing", "stale", "obsolete", "unchanged"}:
            raise SyncError(f"generated catalog report fields are invalid for '{locale}'")
        for key in ("missing", "stale", "obsolete", "unchanged"):
            values = status[key]
            if not isinstance(values, list) or values != sorted(set(values)):
                raise SyncError(f"generated catalog report '{locale}.{key}' is not stable")


def atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    os.replace(temporary, path)


def resolve_lupdate(value: str) -> Path:
    path = Path(value).resolve()
    if not path.is_file():
        raise SyncError(f"lupdate executable not found: {path}")
    return path


def synchronize(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    source_dir = (root / args.source_dir).resolve()
    catalog_dir = (root / args.catalog_dir).resolve()
    output_dir = (root / args.output_dir).resolve()
    source_catalog = (root / args.source_catalog).resolve()
    if not source_dir.is_dir():
        raise SyncError(f"source directory not found: {source_dir}")
    if not catalog_dir.is_dir():
        raise SyncError(f"catalog directory not found: {catalog_dir}")

    scanned = scan_source_ids(root, source_dir)
    extracted = run_lupdate(resolve_lupdate(args.lupdate), source_dir)
    manifest = build_manifest(scanned, extracted)
    manifest_path = output_dir / "messages.json"
    report_path = output_dir / "catalog-status.json"

    catalogs = sorted(catalog_dir.glob("*.ts"))
    if source_catalog not in catalogs:
        raise SyncError(f"source catalog is not tracked below the catalog directory: {source_catalog}")

    desired: dict[Path, bytes] = {manifest_path: json_bytes(manifest)}
    locales: dict[str, dict[str, object]] = {}
    summaries: list[tuple[Path, dict[str, int]]] = []
    for catalog in catalogs:
        catalog_root = parse_catalog(catalog)
        locale = locale_tag(catalog_root, catalog)
        if locale in locales:
            raise SyncError(f"multiple catalogs declare locale '{locale}'")
        content, status, changes = synchronize_catalog(
            catalog, catalog_root, manifest, args.source_locale
        )
        desired[catalog] = content
        locales[locale] = {
            "catalog": catalog.relative_to(root).as_posix(),
            **status,
        }
        summaries.append((catalog, changes))

    report = {
        "$schema": "../schema/catalog-status.schema.json",
        "schema_version": 1,
        "source_manifest": manifest_path.relative_to(root).as_posix(),
        "locales": {locale: locales[locale] for locale in sorted(locales)},
    }
    validate_report(report)
    desired[report_path] = json_bytes(report)

    divergent = sorted(
        path for path, content in desired.items()
        if not path.is_file() or path.read_bytes() != content
    )
    if args.check:
        if divergent:
            print("i18n synchronization required:", file=sys.stderr)
            for path in divergent:
                print(f"  {path.relative_to(root).as_posix()}", file=sys.stderr)
            return 1
        print("i18n catalogs and extraction metadata are current")
        return 0

    for path in divergent:
        atomic_write(path, desired[path])
    for catalog, changes in summaries:
        print(
            f"{catalog.relative_to(root).as_posix()}: "
            f"added={changes['added']} changed={changes['changed']} "
            f"obsolete={changes['obsolete']} unchanged={changes['unchanged']}"
        )
    print(f"updated {len(divergent)} file(s)")
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--root", required=True, help="project root")
    result.add_argument("--lupdate", required=True, help="Qt lupdate executable")
    result.add_argument("--check", action="store_true", help="fail without writing on divergence")
    result.add_argument("--source-dir", default="src")
    result.add_argument("--catalog-dir", default="i18n/app")
    result.add_argument("--output-dir", default="i18n/extracted")
    result.add_argument("--source-catalog", default="i18n/app/gamehq_en_US.ts")
    result.add_argument("--source-locale", default="en-US")
    return result


def main() -> int:
    try:
        return synchronize(parser().parse_args())
    except SyncError as error:
        print(f"i18n sync error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
