#!/usr/bin/env python3
"""Build and check the exact translation workset caused by a manifest delta."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import protocol
import sync as extraction
import verify


class DiffCheckError(RuntimeError):
    pass


def canonical_json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def validate_manifest(value: dict[str, object], label: str) -> dict[str, dict[str, object]]:
    try:
        extraction.validate_manifest(value)
    except extraction.SyncError as error:
        raise DiffCheckError(f"{label}: {error}")
    return {str(message["id"]): message for message in value["messages"]}


def classify_delta(
    base_messages: dict[str, dict[str, object]],
    current_messages: dict[str, dict[str, object]],
) -> dict[str, list[str]]:
    base_ids = set(base_messages)
    current_ids = set(current_messages)
    shared = base_ids & current_ids
    return {
        "added": sorted(current_ids - base_ids),
        "changed": sorted(
            message_id for message_id in shared
            if base_messages[message_id]["source_hash"]
            != current_messages[message_id]["source_hash"]
        ),
        "removed": sorted(base_ids - current_ids),
        "unchanged": sorted(
            message_id for message_id in shared
            if base_messages[message_id]["source_hash"]
            == current_messages[message_id]["source_hash"]
        ),
    }


def validate_obsolete_history(
    catalog_status: dict[str, object], removed: list[str]
) -> None:
    try:
        extraction.validate_report(catalog_status)
    except extraction.SyncError as error:
        raise DiffCheckError(f"catalog status: {error}")
    for locale, status in catalog_status["locales"].items():
        absent = sorted(set(removed) - set(status["obsolete"]))
        if absent:
            raise DiffCheckError(
                f"{locale}: removed IDs are absent from obsolete history: {absent}"
            )


def delta_identifier(
    delta: dict[str, list[str]], current_messages: dict[str, dict[str, object]]
) -> str:
    affected = delta["added"] + delta["changed"]
    identity = {
        "added": delta["added"],
        "changed": delta["changed"],
        "removed": delta["removed"],
        "source_hashes": {
            message_id: current_messages[message_id]["source_hash"]
            for message_id in sorted(affected)
        },
    }
    packed = json.dumps(identity, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(packed.encode("utf-8")).hexdigest()[:16]


def queue_unit(
    message: dict[str, object],
    state: dict[str, object],
    payload: dict[str, object] | None,
) -> dict[str, object]:
    locations = message["locations"]
    location = locations[0]
    prior_status = str(state["status"])
    prior_translation = payload["forms"] if prior_status == "stale" and payload else None
    return {
        "id": message["id"],
        "source": message["source"],
        "source_hash": message["source_hash"],
        "context": message["context"],
        "location_hint": f"{location['file']}:{location['line']}",
        "domain": message["domain"],
        "plural": message["plural"],
        "placeholders": message["placeholders"],
        "markup_signature": message["markup_signature"],
        "accelerator_count": verify.accelerator_count(str(message["source"])),
        "protected_tokens": message["protected_tokens"],
        "prior_translation": prior_translation,
        "prior_status": prior_status,
    }


def build_workset(
    *,
    base_label: str,
    source_manifest: str,
    base_messages: dict[str, dict[str, object]],
    current_messages: dict[str, dict[str, object]],
    registry: dict[str, object],
    state: dict[str, object],
    payloads: dict[str, dict[str, dict[str, object] | None]],
    policy_root: Path,
) -> dict[str, object]:
    delta = classify_delta(base_messages, current_messages)
    affected = sorted(delta["added"] + delta["changed"])
    delta_id = delta_identifier(delta, current_messages)
    source_locale = registry.get("source_language")
    if source_locale != "en-US":
        raise DiffCheckError("locale registry source_language must be en-US")
    registry_locales = registry.get("locales")
    state_locales = state.get("locales")
    if not isinstance(registry_locales, list) or not isinstance(state_locales, dict):
        raise DiffCheckError("locale registry or translation state is malformed")

    locale_work: dict[str, object] = {}
    for locale in sorted(
        (
            value for value in registry_locales
            if isinstance(value, dict)
            and value.get("state") == "enabled"
            and value.get("tag") != source_locale
        ),
        key=lambda value: str(value["tag"]),
    ):
        tag = str(locale["tag"])
        locale_state = state_locales.get(tag)
        if not isinstance(locale_state, dict) or not isinstance(locale_state.get("messages"), dict):
            raise DiffCheckError(f"{tag}: enabled locale is absent from translation state")
        messages_state = locale_state["messages"]
        locale_payloads = payloads.get(tag, {})
        complete: list[str] = []
        required: list[str] = []
        units: list[dict[str, object]] = []
        for message_id in affected:
            message = current_messages[message_id]
            message_state = messages_state.get(message_id)
            if not isinstance(message_state, dict):
                raise DiffCheckError(f"{tag}/{message_id}: missing translation state")
            current = (
                message_state.get("status") in verify.CURRENT_STATUSES
                and message_state.get("source_hash") == message["source_hash"]
            )
            if current:
                complete.append(message_id)
            else:
                required.append(message_id)
                units.append(queue_unit(message, message_state, locale_payloads.get(message_id)))

        queue: dict[str, object] | None = None
        if units:
            glossary, style = protocol.load_policy(policy_root, tag)
            queue = {
                "$schema": "../schema/translation-queue.schema.json",
                "protocol_version": protocol.PROTOCOL_VERSION,
                "batch_id": f"change-{delta_id}.{tag.lower()}",
                "source_locale": source_locale,
                "target_locale": tag,
                "locale": {
                    "direction": locale.get("direction"),
                    "fallback": locale.get("fallback"),
                },
                "glossary_version": glossary["schema_version"],
                "style_version": style["schema_version"],
                "units": units,
            }
            protocol.validate_queue(policy_root, queue)
        locale_work[tag] = {
            "complete": complete,
            "required": required,
            "queue": queue,
            "response_validated": False,
        }

    return {
        "$schema": "../schema/translation-workset.schema.json",
        "schema_version": 1,
        "base": base_label,
        "source_manifest": source_manifest,
        "delta_id": delta_id,
        "delta": delta,
        "locales": locale_work,
    }


def validate_response_directory(workset: dict[str, object], response_dir: Path) -> None:
    if not response_dir.is_dir():
        raise DiffCheckError(f"response directory not found: {response_dir}")
    locales = workset["locales"]
    files = sorted(response_dir.glob("*.json"))
    for path in files:
        tag = path.stem
        locale_work = locales.get(tag)
        if not isinstance(locale_work, dict) or locale_work.get("queue") is None:
            raise DiffCheckError(f"{path.name}: response has no requested locale workset")
        response = protocol.read_json(path)
        queue = locale_work["queue"]
        queued = {str(unit["id"]): unit for unit in queue["units"]}
        try:
            protocol.validate_response(queue, queued, response)
        except (protocol.ProtocolError, verify.VerificationError) as error:
            raise DiffCheckError(f"{path.name}: {error}")
        locale_work["response_validated"] = True


def is_complete(workset: dict[str, object]) -> bool:
    return all(not value["required"] for value in workset["locales"].values())


def read_git_manifest(root: Path, reference: str, relative_path: Path) -> dict[str, object]:
    process = subprocess.run(
        ["git", "-C", str(root), "show", f"{reference}:{relative_path.as_posix()}"],
        capture_output=True,
    )
    if process.returncode:
        detail = process.stderr.decode("utf-8", errors="replace").strip()
        raise DiffCheckError(f"cannot read base manifest from {reference}: {detail}")
    try:
        value = json.loads(process.stdout.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DiffCheckError(f"base manifest from {reference} is invalid UTF-8 JSON: {error}")
    if not isinstance(value, dict):
        raise DiffCheckError(f"base manifest from {reference} must be an object")
    return value


def load_payloads(
    root: Path,
    messages: dict[str, dict[str, object]],
    state: dict[str, object],
) -> dict[str, dict[str, dict[str, object] | None]]:
    result: dict[str, dict[str, dict[str, object] | None]] = {}
    for tag, locale_state in state["locales"].items():
        if not locale_state["catalog_present"]:
            result[tag] = {message_id: None for message_id in messages}
            continue
        catalog = root / str(locale_state["catalog"])
        result[tag] = verify.read_catalog(catalog, tag, messages)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    base = parser.add_mutually_exclusive_group()
    base.add_argument("--base-ref", default="HEAD")
    base.add_argument("--base-manifest", type=Path)
    parser.add_argument("--manifest", type=Path, default=Path("i18n/extracted/messages.json"))
    parser.add_argument("--registry", type=Path, default=Path("i18n/locales.json"))
    parser.add_argument("--state", type=Path, default=Path("i18n/state/translations.json"))
    parser.add_argument("--catalog-status", type=Path, default=Path("i18n/extracted/catalog-status.json"))
    parser.add_argument("--policy-root", type=Path)
    parser.add_argument("--response-dir", type=Path)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    try:
        root = arguments.root.resolve()
        policy_root = arguments.policy_root.resolve() if arguments.policy_root else root
        manifest_path = (root / arguments.manifest).resolve()
        current_manifest = protocol.read_json(manifest_path)
        current_messages = validate_manifest(current_manifest, "current manifest")
        if arguments.base_manifest:
            base_path = arguments.base_manifest.resolve()
            base_manifest = protocol.read_json(base_path)
            base_label = base_path.as_posix()
        else:
            base_manifest = read_git_manifest(root, arguments.base_ref, arguments.manifest)
            base_label = f"git:{arguments.base_ref}"
        base_messages = validate_manifest(base_manifest, "base manifest")

        registry_path = (root / arguments.registry).resolve()
        state_path = (root / arguments.state).resolve()
        registry = protocol.read_json(registry_path)
        state = protocol.read_json(state_path)
        desired_state = verify.reconcile(root, manifest_path, registry_path, state_path)
        if state_path.read_bytes() != verify.json_bytes(desired_state):
            raise DiffCheckError(
                "translation state is not current; run tools/i18n/verify.ps1 -UpdateState"
            )
        catalog_status = protocol.read_json((root / arguments.catalog_status).resolve())
        delta = classify_delta(base_messages, current_messages)
        validate_obsolete_history(catalog_status, delta["removed"])
        payloads = load_payloads(root, current_messages, state)
        workset = build_workset(
            base_label=base_label,
            source_manifest=arguments.manifest.as_posix(),
            base_messages=base_messages,
            current_messages=current_messages,
            registry=registry,
            state=state,
            payloads=payloads,
            policy_root=policy_root,
        )
        if arguments.response_dir:
            validate_response_directory(workset, arguments.response_dir.resolve())
        if arguments.output:
            extraction.atomic_write(arguments.output.resolve(), canonical_json_bytes(workset))

        affected = workset["delta"]["added"] + workset["delta"]["changed"]
        pending = sum(len(value["required"]) for value in workset["locales"].values())
        print(
            f"translation delta {workset['delta_id']}: affected={len(affected)} "
            f"removed={len(workset['delta']['removed'])} pending={pending}"
        )
        if pending:
            for tag, value in workset["locales"].items():
                if value["required"]:
                    print(f"  {tag}: {', '.join(value['required'])}", file=sys.stderr)
            return 1
        print("localization delta is complete")
        return 0
    except (
        DiffCheckError,
        protocol.ProtocolError,
        verify.VerificationError,
        extraction.SyncError,
    ) as error:
        print(f"i18n diff check error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
