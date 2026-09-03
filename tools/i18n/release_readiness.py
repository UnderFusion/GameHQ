#!/usr/bin/env python3
"""Generate and validate deterministic localization release-readiness evidence."""

from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
PRODUCTION_COUNT = 16
REQUIRED_SURFACES = (
    "gamehq.navigation.about",
    "gamehq.navigation.support_gamehq",
)
PSEUDO_LOCALES = ("en-XA", "ar-XB")
RESERVE_LOCALES = ("cs-CZ",)
HASH_PATTERN = re.compile(r"^[0-9a-f]{64}$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
FORBIDDEN_KEYS = {
    "conversation",
    "conversation_contents",
    "credentials",
    "prompt",
    "response_text",
    "secrets",
    "telemetry",
    "user_data",
}


class ReadinessError(RuntimeError):
    pass


def read_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_bytes().decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReadinessError(f"cannot read strict UTF-8 JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise ReadinessError(f"JSON root must be an object: {path}")
    return value


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def file_evidence(root: Path, relative: str) -> dict[str, object]:
    path = root / relative
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise ReadinessError(f"cannot read readiness input {relative}: {error}") from error
    return {"path": relative.replace("\\", "/"), "sha256": sha256_bytes(payload)}


def translation_hash(forms: list[str]) -> str:
    payload = json.dumps(forms, ensure_ascii=False, separators=(",", ":"))
    return sha256_bytes(payload.encode("utf-8"))


def catalog_translation_hash(path: Path, message_id: str) -> str:
    try:
        root = ET.fromstring(path.read_bytes().decode("utf-8"))
    except (OSError, UnicodeDecodeError, ET.ParseError) as error:
        raise ReadinessError(f"cannot read catalog {path}: {error}") from error
    for message in root.findall(".//message"):
        if message.get("id") != message_id:
            continue
        translation = message.find("translation")
        if translation is None or translation.get("type") == "unfinished":
            raise ReadinessError(f"{path}: {message_id} is not translated")
        numerus = translation.findall("numerusform")
        forms = ["".join(value.itertext()) for value in numerus]
        if not forms:
            forms = ["".join(translation.itertext())]
        if any(not form.strip() for form in forms):
            raise ReadinessError(f"{path}: {message_id} contains an empty form")
        return translation_hash(forms)
    raise ReadinessError(f"{path}: missing required surface {message_id}")


def reject_private_fields(value: object, label: str = "root") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if str(key).casefold() in FORBIDDEN_KEYS:
                raise ReadinessError(f"{label}: forbidden private field {key!r}")
            reject_private_fields(child, f"{label}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_private_fields(child, f"{label}[{index}]")


def validate_corrections(
    corrections: dict[str, object], root: Path, production_tags: list[str],
    extracted: dict[str, object], locale_entries: dict[str, dict[str, object]],
) -> None:
    reject_private_fields(corrections, "corrections")
    required_root = {"$schema", "schema_version", "entries"}
    if set(corrections) != required_root or corrections.get("schema_version") != 1:
        raise ReadinessError("corrections: invalid version-one document shape")
    entries = corrections.get("entries")
    if not isinstance(entries, list):
        raise ReadinessError("corrections.entries must be an array")
    messages = {str(value["id"]): value for value in extracted.get("messages", [])
                if isinstance(value, dict) and "id" in value}
    seen: set[str] = set()
    required = {
        "correction_id", "locale", "message_id", "source_hash",
        "before_translation_hash", "after_translation_hash", "reason", "method",
        "actor", "review_state", "recorded_at", "commit",
    }
    for index, entry in enumerate(entries):
        label = f"corrections.entries[{index}]"
        if not isinstance(entry, dict) or set(entry) != required:
            raise ReadinessError(f"{label}: invalid correction shape")
        correction_id = str(entry["correction_id"])
        if not correction_id or correction_id in seen:
            raise ReadinessError(f"{label}: correction_id must be non-empty and unique")
        seen.add(correction_id)
        locale = str(entry["locale"])
        message_id = str(entry["message_id"])
        if locale not in production_tags:
            raise ReadinessError(f"{label}: unsupported production locale {locale}")
        if message_id not in messages:
            raise ReadinessError(f"{label}: unknown message ID {message_id}")
        if entry["source_hash"] != messages[message_id].get("source_hash"):
            raise ReadinessError(f"{label}: stale source hash")
        for field in ("source_hash", "before_translation_hash", "after_translation_hash"):
            if not HASH_PATTERN.fullmatch(str(entry[field])):
                raise ReadinessError(f"{label}.{field}: invalid SHA-256")
        if entry["before_translation_hash"] == entry["after_translation_hash"]:
            raise ReadinessError(f"{label}: correction must change the translation")
        catalog_path = root / str(locale_entries[locale]["catalog"])
        if entry["after_translation_hash"] != catalog_translation_hash(catalog_path, message_id):
            raise ReadinessError(f"{label}: after hash does not match the current catalog")
        if entry["review_state"] not in {"pending_linguistic_qa", "linguistically_accepted"}:
            raise ReadinessError(f"{label}: invalid review state")
        if entry["review_state"] == "linguistically_accepted" \
                and entry["method"] != "human_contextual_review":
            raise ReadinessError(f"{label}: only human contextual review may record acceptance")
        if not all(isinstance(entry[field], str) and str(entry[field]).strip()
                   for field in ("reason", "method", "actor")):
            raise ReadinessError(f"{label}: reason, method, and actor are required")
        try:
            datetime.fromisoformat(str(entry["recorded_at"]).replace("Z", "+00:00"))
        except ValueError as error:
            raise ReadinessError(f"{label}.recorded_at: invalid ISO-8601 timestamp") from error
        if not COMMIT_PATTERN.fullmatch(str(entry["commit"])):
            raise ReadinessError(f"{label}.commit: expected a full commit SHA")


def build_evidence(root: Path) -> dict[str, object]:
    locale_manifest = read_json(root / "i18n/locales.json")
    release_manifest = read_json(root / "assets/release-notes/manifest.json")
    translation_state = read_json(root / "i18n/state/translations.json")
    extracted = read_json(root / "i18n/extracted/messages.json")
    corrections = read_json(root / "i18n/release/corrections.json")
    enabled = [entry for entry in locale_manifest.get("locales", [])
               if isinstance(entry, dict) and entry.get("state") == "enabled"]
    production_tags = [str(entry["tag"]) for entry in enabled]
    if len(production_tags) != PRODUCTION_COUNT or len(set(production_tags)) != PRODUCTION_COUNT:
        raise ReadinessError("portfolio must contain exactly 16 unique production locales")
    if any(tag in production_tags for tag in (*PSEUDO_LOCALES, *RESERVE_LOCALES)):
        raise ReadinessError("pseudo and reserve locales cannot enter the production portfolio")
    if release_manifest.get("production_locales") != production_tags:
        raise ReadinessError("release-note and application locale portfolios differ")
    launch = release_manifest.get("localization_launch")
    if not isinstance(launch, dict) or launch.get("status") != "designated" \
            or launch.get("designated_by") != "owner" or launch.get("date") is not None:
        raise ReadinessError("localization launch must remain owner-designated and date-null")
    version = str(launch.get("version"))
    locale_entries: dict[str, dict[str, object]] = {}
    state_locales = translation_state.get("locales")
    if not isinstance(state_locales, dict):
        raise ReadinessError("translation state has no locale map")
    for manifest_entry in enabled:
        tag = str(manifest_entry["tag"])
        state = state_locales.get(tag)
        if not isinstance(state, dict):
            raise ReadinessError(f"translation state missing production locale {tag}")
        messages = state.get("messages")
        if not isinstance(messages, dict):
            raise ReadinessError(f"translation state missing message map for {tag}")
        catalog = str(state.get("catalog"))
        required = []
        for message_id in REQUIRED_SURFACES:
            message = messages.get(message_id)
            if not isinstance(message, dict):
                raise ReadinessError(f"{tag}: missing readiness surface {message_id}")
            required.append({
                "id": message_id,
                "source_hash": message.get("source_hash"),
                "catalog_translation_hash": catalog_translation_hash(root / catalog, message_id),
                "recorded_translation_hash": message.get("translation_hash"),
                "translation_state": message.get("status"),
                "provenance": message.get("provenance"),
                "updated_at": message.get("updated_at"),
            })
        status_counts = Counter(str(value.get("status")) for value in messages.values()
                                if isinstance(value, dict))
        provenance_counts = Counter(str(value.get("provenance", {}).get("kind"))
                                    for value in messages.values() if isinstance(value, dict))
        locale_entries[tag] = {
            "catalog": catalog,
            "catalog_sha256": file_evidence(root, catalog)["sha256"],
            "style": file_evidence(root, f"i18n/style/{tag}.json"),
            "launch_release_note": file_evidence(
                root, f"assets/release-notes/versions/{version}/{tag}.json"),
            "translation_state_counts": dict(sorted(status_counts.items())),
            "provenance_counts": dict(sorted(provenance_counts.items())),
            "required_surfaces": required,
            "linguistic_qa": {"state": "pending", "authority": "p8-3", "artifact": None},
        }
    validate_corrections(corrections, root, production_tags, extracted, locale_entries)
    evidence = {
        "$schema": "../schema/release-readiness.schema.json",
        "schema_version": 1,
        "candidate": {
            "version": version,
            "date": None,
            "designation": "owner",
            "repository_version": (root / "VERSION").read_text(encoding="utf-8").strip(),
            "release_authorization": "not_requested",
            "publication_state": "prohibited",
        },
        "source_freeze": [
            file_evidence(root, "VERSION"),
            file_evidence(root, "i18n/locales.json"),
            file_evidence(root, "i18n/extracted/messages.json"),
            file_evidence(root, "i18n/app/gamehq_en_US.ts"),
            file_evidence(root, "assets/release-notes/manifest.json"),
            file_evidence(root, f"assets/release-notes/versions/{version}/en-US.json"),
            file_evidence(root, "i18n/release/corrections.json"),
        ],
        "portfolio": {
            "production_locales": production_tags,
            "development_only": list(PSEUDO_LOCALES),
            "reserve": list(RESERVE_LOCALES),
        },
        "workflow": [
            {"order": 1, "id": "freeze-english", "gate": "source hashes match"},
            {"order": 2, "id": "synchronize", "gate": "tools/i18n/sync.ps1 -Check"},
            {"order": 3, "id": "translate-queue", "gate": "only missing or stale units"},
            {"order": 4, "id": "verify-structure-provenance", "gate": "all 16 production locales"},
            {"order": 5, "id": "generate-package", "gate": "p7-1 and p7-2"},
            {"order": 6, "id": "automated-acceptance", "gate": "p8-1 then p8-2"},
            {"order": 7, "id": "contextual-linguistic-qa", "gate": "p8-3 only"},
            {"order": 8, "id": "owner-release-authorization", "gate": "p8-4 only"},
        ],
        "provenance_contract": {
            "required_fields": [
                "source_hash", "locale", "method", "actor", "translation_state",
                "review_state", "recorded_at", "correction_history",
            ],
            "forbidden_fields": sorted(FORBIDDEN_KEYS),
            "provider_identity": "opaque-workflow-label-only",
            "machine_output_self_certifies_linguistic_quality": False,
        },
        "correction_contract": {
            "translation_mode": "auditable-delta-against-current-source",
            "released_history": "immutable-except-explicit-correction",
            "translation_ledger": "i18n/release/corrections.json",
            "release_note_ledger": "assets/release-notes/manifest.json",
        },
        "locales": locale_entries,
        "handoff": [
            {"item": "p8-1", "state": "pending", "authority": "asset-and-provenance"},
            {"item": "p8-2", "state": "pending", "authority": "automated-acceptance"},
            {"item": "p8-3", "state": "pending", "authority": "linguistic-acceptance"},
            {"item": "p8-4", "state": "pending", "authority": "owner-release-authorization"},
        ],
    }
    reject_private_fields(evidence, "readiness")
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    output = arguments.output or root / "i18n/release/readiness-0.7.7.json"
    if not output.is_absolute():
        output = root / output
    try:
        payload = json_bytes(build_evidence(root))
        if arguments.check:
            if not output.is_file() or output.read_bytes() != payload:
                raise ReadinessError(f"stale release-readiness evidence: {output}")
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(payload)
    except ReadinessError as error:
        print(f"release-readiness error: {error}", file=sys.stderr)
        return 1
    print("release readiness verified: 16 locales, p8 gates pending, publication prohibited")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
