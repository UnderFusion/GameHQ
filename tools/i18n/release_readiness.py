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

import linguistic_qa


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
VERSION_PATTERN = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
INTEGRITY_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")
ISO_DATE_PATTERN = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
CANDIDATE_MODE = "candidate"
FINAL_MODE = "final"
MODES = (CANDIDATE_MODE, FINAL_MODE)
RELEASE_ENTRY_FIELDS = (
    "version", "date", "status", "localization_policy",
    "source_integrity", "original_source_integrity", "correction",
)
SOURCE_REVIEWED = "source_reviewed"
CONTEXTUALLY_REVIEWED = "contextually_reviewed"
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
        if entry["review_state"] not in {
            "pending_linguistic_qa", "contextually_reviewed", "linguistically_accepted"
        }:
            raise ReadinessError(f"{label}: invalid review state")
        if entry["review_state"] == "contextually_reviewed" \
                and entry["method"] != "agent_contextual_review":
            raise ReadinessError(f"{label}: agent contextual review requires its explicit method")
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


def repository_version(root: Path) -> str:
    return (root / "VERSION").read_text(encoding="utf-8").strip()


def parse_version(value: object, label: str) -> str:
    if not isinstance(value, str) or not VERSION_PATTERN.fullmatch(value):
        raise ReadinessError(f"{label}: invalid release version {value!r}")
    return value


def version_key(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in value.split("."))


def parse_date(value: object, label: str) -> str:
    if not isinstance(value, str) or not ISO_DATE_PATTERN.fullmatch(value):
        raise ReadinessError(f"{label}: expected an ISO-8601 calendar date")
    try:
        datetime.strptime(value, "%Y-%m-%d")
    except ValueError as error:
        raise ReadinessError(f"{label}: {value} is not a real calendar date") from error
    return value


def candidate_state(root: Path, launch: dict[str, object]) -> dict[str, object]:
    """Validate the pre-release candidate state the owner gate protects."""
    if launch.get("status") != "designated" or launch.get("designated_by") != "owner" \
            or launch.get("date") is not None:
        raise ReadinessError("localization launch must remain owner-designated and date-null")
    return {
        "version": str(launch.get("version")),
        "date": None,
        "designation": "owner",
        "repository_version": repository_version(root),
        "release_authorization": "not_requested",
        "publication_state": "prohibited",
    }


def release_note_integrity(document: dict[str, object]) -> str:
    """Recompute the canonical release-note source integrity.

    This intentionally mirrors generate_release_notes.source_integrity so the
    readiness gate stays import-light; test_release_readiness pins both
    implementations to the same hash so the duplication cannot drift.
    """
    payload = {key: document.get(key) for key in ("version", "date", "sections")}
    canonical = json.dumps(
        payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(canonical).hexdigest()


def release_history_entry(
    root: Path, manifest: dict[str, object], version: str, date: str
) -> dict[str, object]:
    """Return the unique release-history entry a finalized version must carry.

    Generation and publication ship exactly what releases[] records, so a
    released localization launch is only finalized once it has been promoted
    into the history: exactly once, at the documented newest-first position,
    with metadata and integrity that match the dated English source document.
    """
    releases = manifest.get("releases")
    if not isinstance(releases, list) or not releases:
        raise ReadinessError("release-note manifest records no release history")
    keys: list[tuple[int, ...]] = []
    for index, entry in enumerate(releases):
        if not isinstance(entry, dict):
            raise ReadinessError(f"manifest.releases[{index}] is not an object")
        keys.append(version_key(
            parse_version(entry.get("version"), f"manifest.releases[{index}].version")))
    if len(set(keys)) != len(keys):
        raise ReadinessError("release history records a version more than once")
    if keys != sorted(keys, reverse=True):
        raise ReadinessError("release history is not deterministically newest-first")
    current = version_key(version)
    matching = [entry for entry in releases if entry.get("version") == version]
    if not matching:
        raise ReadinessError(
            f"released version {version} was never promoted into the release history")
    if keys[0] != current:
        raise ReadinessError(f"released version {version} is not the newest release-history entry")
    entry = matching[0]
    if set(entry) != set(RELEASE_ENTRY_FIELDS):
        raise ReadinessError(f"release history entry {version} does not match the manifest schema")
    if entry.get("status") != "released":
        raise ReadinessError(f"release history entry {version} is not marked released")
    if entry.get("date") != date:
        raise ReadinessError(
            f"release history entry {version} records {entry.get('date')!r} "
            f"instead of the launch date {date}")
    if entry.get("localization_policy") != "complete":
        raise ReadinessError(
            f"release history entry {version} must require all sixteen locales")
    for field in ("source_integrity", "original_source_integrity"):
        if not INTEGRITY_PATTERN.fullmatch(str(entry.get(field))):
            raise ReadinessError(f"release history entry {version}: invalid {field}")
    relative = f"assets/release-notes/versions/{version}/en-US.json"
    integrity = release_note_integrity(read_json(root / relative))
    if entry["source_integrity"] != integrity:
        raise ReadinessError(
            f"release history entry {version}: source_integrity does not match {relative}")
    # A release being published for the first time cannot already be recorded as
    # a correction of itself; corrected history is a separate lifecycle.
    if entry["original_source_integrity"] != integrity:
        raise ReadinessError(
            f"release history entry {version}: original_source_integrity does not match {relative}")
    if entry["correction"] is not None:
        raise ReadinessError(
            f"release history entry {version} records a correction before it was ever released")
    return {field: entry[field] for field in RELEASE_ENTRY_FIELDS}


def final_state(
    root: Path, manifest: dict[str, object], launch: dict[str, object]
) -> dict[str, object]:
    """Validate a repository that has already been finalized elsewhere.

    Selecting final mode is not owner authorization. This path only proves that
    the repository consistently describes its VERSION as a released, fully
    localized version that was promoted into the release history exactly once;
    it never records owner intent, never authorizes publication, and never
    writes repository metadata.
    """
    if launch.get("status") != "released":
        raise ReadinessError("final mode requires a released localization launch")
    if launch.get("designated_by") != "owner":
        raise ReadinessError("the released localization launch must stay owner-designated")
    if launch.get("localization_policy") != "complete":
        raise ReadinessError("the released localization launch must require all sixteen locales")
    version = parse_version(launch.get("version"), "localization_launch.version")
    date = parse_date(launch.get("date"), "localization_launch.date")
    recorded = repository_version(root)
    if recorded != version:
        raise ReadinessError(
            f"repository VERSION {recorded} differs from the released launch version {version}")
    return {
        "version": version,
        "date": date,
        "designation": "owner",
        "repository_version": recorded,
        "release_authorization": "not_validated",
        "publication_state": "requires_owner_authorization",
        "release_entry": release_history_entry(root, manifest, version, date),
    }


def validate_final_localization(
    root: Path, version: str, date: str, production_tags: list[str],
    locale_entries: dict[str, dict[str, object]],
) -> None:
    """Require finished localization evidence for an already finalized version."""
    pending = sorted(tag for tag, value in locale_entries.items()
                     if value["linguistic_qa"]["state"] != CONTEXTUALLY_REVIEWED)
    if pending:
        raise ReadinessError(
            f"final mode requires contextual linguistic review for {', '.join(pending)}")
    for tag in production_tags:
        relative = f"assets/release-notes/versions/{version}/{tag}.json"
        document = read_json(root / relative)
        if document.get("version") != version:
            raise ReadinessError(
                f"{relative}: records version {document.get('version')!r} instead of {version}")
        if document.get("date") != date:
            raise ReadinessError(
                f"{relative}: records date {document.get('date')!r} "
                f"instead of the release date {date}")
    linguistic = read_json(root / "assets/release-notes/linguistic-state.json")
    if linguistic.get("candidate") != version:
        raise ReadinessError(
            f"release-note linguistic state tracks {linguistic.get('candidate')!r} "
            f"instead of {version}")
    units = linguistic.get("units")
    if not isinstance(units, dict) or not units:
        raise ReadinessError("release-note linguistic state records no reviewed units")
    for unit_id, unit in sorted(units.items()):
        locales = unit.get("locales") if isinstance(unit, dict) else None
        if not isinstance(locales, dict) or set(locales) != set(production_tags):
            raise ReadinessError(
                f"release-note unit {unit_id} does not cover all sixteen production locales")
        for tag, state in sorted(locales.items()):
            expected = SOURCE_REVIEWED if tag == "en-US" else CONTEXTUALLY_REVIEWED
            if state != expected:
                raise ReadinessError(
                    f"release-note unit {unit_id}: {tag} is {state!r}, expected {expected!r}")


def build_evidence(root: Path, mode: str = CANDIDATE_MODE) -> dict[str, object]:
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
    if mode not in MODES:
        raise ReadinessError(f"unsupported validation mode {mode!r}")
    launch = release_manifest.get("localization_launch")
    if not isinstance(launch, dict):
        raise ReadinessError("release-note manifest has no localization launch")
    release_state = candidate_state(root, launch) if mode == CANDIDATE_MODE \
        else final_state(root, release_manifest, launch)
    version = str(release_state["version"])
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
        review_path = root / f"i18n/quality/reviews/{tag}.json"
        if review_path.is_file():
            try:
                review = linguistic_qa.validate_review(root, review_path)
            except linguistic_qa.ReviewError as error:
                raise ReadinessError(f"{tag}: invalid linguistic review: {error}") from error
            qa = {
                "state": str(review["review"]["state"]),
                "authority": "p8-3",
                "artifact": file_evidence(root, review_path.relative_to(root).as_posix()),
            }
        else:
            qa = {"state": "pending", "authority": "p8-3", "artifact": None}
        locale_entries[tag] = {
            "catalog": catalog,
            "catalog_sha256": file_evidence(root, catalog)["sha256"],
            "style": file_evidence(root, f"i18n/style/{tag}.json"),
            "launch_release_note": file_evidence(
                root, f"assets/release-notes/versions/{version}/{tag}.json"),
            "translation_state_counts": dict(sorted(status_counts.items())),
            "provenance_counts": dict(sorted(provenance_counts.items())),
            "required_surfaces": required,
            "linguistic_qa": qa,
        }
    validate_corrections(corrections, root, production_tags, extracted, locale_entries)
    if mode == FINAL_MODE:
        validate_final_localization(
            root, version, str(release_state["date"]), production_tags, locale_entries)
    evidence: dict[str, object] = {
        "$schema": "../schema/release-readiness.schema.json",
        "schema_version": 1,
    }
    if mode == FINAL_MODE:
        evidence["mode"] = FINAL_MODE
        evidence["release"] = release_state
    else:
        evidence["candidate"] = release_state
    evidence |= {
        "source_freeze": [
            file_evidence(root, "VERSION"),
            file_evidence(root, "i18n/locales.json"),
            file_evidence(root, "i18n/extracted/messages.json"),
            file_evidence(root, "i18n/app/gamehq_en_US.ts"),
            file_evidence(root, "assets/release-notes/manifest.json"),
            file_evidence(root, f"assets/release-notes/versions/{version}/en-US.json"),
            file_evidence(root, "i18n/release/corrections.json"),
            file_evidence(root, "assets/release-notes/linguistic-state.json"),
        ] + [file_evidence(root, f"i18n/quality/reviews/{tag}.json")
             for tag in production_tags
             if (root / f"i18n/quality/reviews/{tag}.json").is_file()],
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
    parser.add_argument(
        "--mode", choices=MODES, default=CANDIDATE_MODE,
        help="candidate validates the protected pre-release state (default); final validates a "
             "repository that has already been finalized by a separately authorized owner action",
    )
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    if arguments.mode == FINAL_MODE and arguments.output is None:
        print("release-readiness error: final mode requires an explicit --output path",
              file=sys.stderr)
        return 1
    output = arguments.output or root / "i18n/release/readiness-0.7.7.json"
    if not output.is_absolute():
        output = root / output
    try:
        payload = json_bytes(build_evidence(root, arguments.mode))
        if arguments.check:
            if not output.is_file() or output.read_bytes() != payload:
                raise ReadinessError(f"stale release-readiness evidence: {output}")
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(payload)
    except ReadinessError as error:
        print(f"release-readiness error: {error}", file=sys.stderr)
        return 1
    if arguments.mode == FINAL_MODE:
        print("release finalization verified: 16 locales, coherent released repository state, "
              "publication still requires explicit owner authorization")
    else:
        print("release readiness verified: 16 locales, p8 gates pending, publication prohibited")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
