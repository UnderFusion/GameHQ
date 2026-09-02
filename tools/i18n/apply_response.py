#!/usr/bin/env python3
"""Atomically apply validated translation responses to only queued TS entries."""

from __future__ import annotations

import argparse
import copy
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

import check_diff
import protocol
import sync as extraction
import verify


class ApplyError(RuntimeError):
    pass


WORKSET_FIELDS = {
    "$schema", "schema_version", "base", "source_manifest", "delta_id", "delta", "locales",
}
LOCALE_FIELDS = {"complete", "required", "queue", "response_validated"}


def stable_ids(label: str, value: object) -> list[str]:
    if not isinstance(value, list) or value != sorted(set(value)):
        raise ApplyError(f"{label}: IDs must be a uniquely sorted array")
    if any(not isinstance(item, str) or not extraction.ID_PATTERN.fullmatch(item) for item in value):
        raise ApplyError(f"{label}: contains an invalid message ID")
    return value


def validate_workset(workset: dict[str, object]) -> dict[str, dict[str, object]]:
    if set(workset) != WORKSET_FIELDS or workset.get("schema_version") != 1:
        raise ApplyError("workset root does not match schema version 1")
    delta = workset.get("delta")
    if not isinstance(delta, dict) or set(delta) != {"added", "changed", "removed", "unchanged"}:
        raise ApplyError("workset delta fields do not match schema")
    delta_sets: list[set[str]] = []
    for field in ("added", "changed", "removed", "unchanged"):
        delta_sets.append(set(stable_ids(f"workset.delta.{field}", delta[field])))
    for index, left in enumerate(delta_sets):
        if any(left & right for right in delta_sets[index + 1:]):
            raise ApplyError("workset delta classifications overlap")
    affected = set(delta["added"]) | set(delta["changed"])
    locales = workset.get("locales")
    if not isinstance(locales, dict):
        raise ApplyError("workset.locales must be an object")
    normalized: dict[str, dict[str, object]] = {}
    for tag, value in locales.items():
        if not isinstance(tag, str) or not isinstance(value, dict) or set(value) != LOCALE_FIELDS:
            raise ApplyError(f"workset locale {tag!r} does not match schema")
        complete = stable_ids(f"workset.locales.{tag}.complete", value["complete"])
        required = stable_ids(f"workset.locales.{tag}.required", value["required"])
        if set(complete) & set(required) or set(complete) | set(required) != affected:
            raise ApplyError(f"workset.locales.{tag}: complete/required IDs do not match delta")
        queue = value["queue"]
        if not required and queue is not None:
            raise ApplyError(f"workset.locales.{tag}.queue: unexpected queue for complete locale")
        if required:
            if not isinstance(queue, dict):
                raise ApplyError(f"workset.locales.{tag}.queue: required queue is missing")
            if queue.get("target_locale") != tag:
                raise ApplyError(f"workset.locales.{tag}.queue: target locale differs")
            units = queue.get("units")
            if not isinstance(units, list) or [unit.get("id") for unit in units] != required:
                raise ApplyError(f"workset.locales.{tag}.queue: unit IDs differ from required IDs")
        if type(value["response_validated"]) is not bool:
            raise ApplyError(f"workset.locales.{tag}.response_validated must be boolean")
        normalized[tag] = value
    return normalized


def rooted_path(root: Path, relative: object, label: str) -> Path:
    if not isinstance(relative, str) or not relative:
        raise ApplyError(f"{label}: expected a relative project path")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root)
    except ValueError:
        raise ApplyError(f"{label}: path escapes the project root")
    return candidate


def set_translation(translation: ET.Element, forms: list[str], plural: bool) -> None:
    translation.attrib.pop("type", None)
    for child in list(translation):
        translation.remove(child)
    if plural:
        translation.text = None
        for form in forms:
            ET.SubElement(translation, "numerusform").text = form
    else:
        translation.text = forms[0]


def validate_state(state: dict[str, object], source_locale: str) -> None:
    for tag, locale in state["locales"].items():
        for message_id, entry in locale["messages"].items():
            verify.validate_state_entry(
                f"{tag}/{message_id}", entry, tag == source_locale
            )
        verify.validate_completeness(tag, locale["completeness"])


def prepare_apply(
    *,
    root: Path,
    policy_root: Path,
    workset: dict[str, object],
    response_dir: Path,
    updated_at: str,
    registry_path: Path,
    state_path: Path,
) -> dict[Path, bytes]:
    if not verify.valid_timestamp(updated_at):
        raise ApplyError("updated_at must be an ISO-8601 timestamp with timezone")
    locales_work = validate_workset(workset)
    required_tags = sorted(
        tag for tag, value in locales_work.items() if value["queue"] is not None
    )
    if not response_dir.is_dir():
        raise ApplyError(f"response directory not found: {response_dir}")
    response_files = {path.stem: path for path in sorted(response_dir.glob("*.json"))}
    if sorted(response_files) != required_tags:
        raise ApplyError(
            f"response locale set differs from workset; expected={required_tags}, "
            f"actual={sorted(response_files)}"
        )

    manifest_path = rooted_path(root, workset.get("source_manifest"), "source_manifest")
    manifest = protocol.read_json(manifest_path)
    messages = check_diff.validate_manifest(manifest, "current manifest")
    source_locale, registry_locales = verify.load_registry(registry_path)
    locale_by_tag = {str(locale["tag"]): locale for locale in registry_locales}
    current_state = protocol.read_json(state_path)
    reconciled = verify.reconcile(root, manifest_path, registry_path, state_path)
    if state_path.read_bytes() != verify.json_bytes(reconciled):
        raise ApplyError("translation state is not current before apply")
    desired_state = copy.deepcopy(current_state)

    prepared: dict[Path, bytes] = {}
    for tag in required_tags:
        queue = locales_work[tag]["queue"]
        queued = protocol.validate_queue(policy_root, queue)
        response = protocol.read_json(response_files[tag])
        protocol.validate_response(queue, queued, response)
        locale = locale_by_tag.get(tag)
        if locale is None or tag == source_locale:
            raise ApplyError(f"{tag}: target locale is unavailable or is the source locale")
        catalog_path = rooted_path(
            root, f"i18n/app/{locale['qt_catalog']}.ts", f"{tag} catalog"
        )
        if not catalog_path.is_file():
            raise ApplyError(f"{tag}: catalog does not exist; synchronize it before apply")
        catalog_root = extraction.parse_catalog(catalog_path)
        if extraction.locale_tag(catalog_root, catalog_path) != tag:
            raise ApplyError(f"{tag}: catalog language does not match registry")
        catalog_messages = extraction.catalog_messages(catalog_root, catalog_path)
        locale_state = desired_state["locales"].get(tag)
        if not isinstance(locale_state, dict):
            raise ApplyError(f"{tag}: translation state is missing")
        for unit, result in zip(queue["units"], response["units"], strict=True):
            message_id = str(unit["id"])
            state_entry = locale_state["messages"].get(message_id)
            if not isinstance(state_entry, dict):
                raise ApplyError(f"{tag}/{message_id}: translation state is missing")
            if state_entry.get("status") != unit["prior_status"]:
                raise ApplyError(f"{tag}/{message_id}: workset no longer matches prior state")
            message = catalog_messages.get(message_id)
            if message is None or message.findtext("source", default="") != unit["source"]:
                raise ApplyError(f"{tag}/{message_id}: catalog source differs from queue")
            translation = message.find("translation")
            if translation is None:
                raise ApplyError(f"{tag}/{message_id}: catalog translation node is missing")
            set_translation(translation, list(result["translation"]), bool(unit["plural"]))
            locale_state["messages"][message_id] = {
                "domain": unit["domain"],
                "source_hash": unit["source_hash"],
                "translation_hash": result["translation_hash"],
                "status": result["status"],
                "provenance": result["provenance"],
                "updated_at": updated_at,
            }
        extraction.sort_catalog(catalog_root)
        prepared[catalog_path] = extraction.catalog_bytes(catalog_root)
        locale_state["completeness"] = verify.completeness_for(
            messages, locale_state["messages"]
        )

    validate_state(desired_state, source_locale)
    prepared[state_path] = verify.json_bytes(desired_state)
    return prepared


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--policy-root", type=Path)
    parser.add_argument("--workset", type=Path, required=True)
    parser.add_argument("--response-dir", type=Path, required=True)
    parser.add_argument("--updated-at", required=True)
    parser.add_argument("--registry", type=Path, default=Path("i18n/locales.json"))
    parser.add_argument("--state", type=Path, default=Path("i18n/state/translations.json"))
    arguments = parser.parse_args()
    try:
        root = arguments.root.resolve()
        policy_root = arguments.policy_root.resolve() if arguments.policy_root else root
        state_path = rooted_path(root, arguments.state.as_posix(), "state")
        registry_path = rooted_path(root, arguments.registry.as_posix(), "registry")
        workset = protocol.read_json(arguments.workset.resolve())
        prepared = prepare_apply(
            root=root,
            policy_root=policy_root,
            workset=workset,
            response_dir=arguments.response_dir.resolve(),
            updated_at=arguments.updated_at,
            registry_path=registry_path,
            state_path=state_path,
        )
        for path in sorted(prepared, key=lambda value: (value == state_path, value.as_posix())):
            extraction.atomic_write(path, prepared[path])
            print(f"updated {path.relative_to(root).as_posix()}")
        return 0
    except (
        ApplyError,
        check_diff.DiffCheckError,
        protocol.ProtocolError,
        verify.VerificationError,
        extraction.SyncError,
    ) as error:
        print(f"translation apply error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
