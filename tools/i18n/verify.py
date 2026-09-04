#!/usr/bin/env python3
"""Verify GameHQ translation structure and reconcile deterministic trust state."""

from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import sys
import unicodedata
import xml.etree.ElementTree as ET

import sync as extraction


DOMAINS = ("application", "installer", "release_notes")
STATUSES = {
    "missing",
    "machine_translated",
    "machine_verified",
    "contextually_reviewed",
    "human_reviewed",
    "stale",
    "intentionally_inherited",
}
CURRENT_STATUSES = {
    "machine_translated",
    "machine_verified",
    "contextually_reviewed",
    "human_reviewed",
    "intentionally_inherited",
}
SEPARATED_KOREAN_PARTICLE = re.compile(
    r"\b(?:GameHQ|GameInput|GitHub|Playnite|Windows|Steam|XInput|DualSense)\s+"
    r"(?:에서|으로|은|는|이|가|을|를|에|와|과|로)"
    r"(?=(?:만|도)?(?:\s|[.,!?…:;)]|$))"
)
PROVENANCE_KINDS = {
    "none", "source", "unknown", "machine", "agent", "human", "inherited"
}
HASH_PATTERN = re.compile(r"^[0-9a-f]{64}$")
PLURAL_FORMS = {
    "ar": 6,
    "cs": 3,
    "de": 2,
    "en": 2,
    "es": 2,
    "fr": 2,
    "it": 2,
    "ja": 1,
    "ko": 1,
    "pl": 3,
    "pt": 2,
    "ru": 3,
    "th": 1,
    "tr": 1,
    "uk": 3,
    "zh": 1,
}
VOID_MARKUP = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "param", "source", "track", "wbr",
}
MOJIBAKE_SIGNATURES = ("\ufffd", "Ãƒ", "Ã¢", "â€™", "â€œ", "â€", "ðŸ", "Â ")


class VerificationError(RuntimeError):
    pass


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def read_json(path: Path) -> dict[str, object]:
    try:
        text = path.read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise VerificationError(f"cannot read UTF-8 JSON {path}: {error}")
    try:
        value = json.loads(text)
    except json.JSONDecodeError as error:
        raise VerificationError(f"cannot parse JSON {path}: {error}")
    if not isinstance(value, dict):
        raise VerificationError(f"JSON root must be an object: {path}")
    return value


def normalized_tag(value: str) -> str:
    return value.replace("_", "-")


def validate_text(label: str, text: str) -> None:
    if unicodedata.normalize("NFC", text) != text:
        raise VerificationError(f"{label}: text is not NFC-normalized")
    for char in text:
        if unicodedata.category(char) == "Cc" and char not in "\t\n\r":
            raise VerificationError(f"{label}: contains a forbidden control character")
    for signature in MOJIBAKE_SIGNATURES:
        if signature in text:
            raise VerificationError(f"{label}: contains mojibake signature {signature!r}")


def validate_markup(label: str, text: str) -> None:
    stack: list[str] = []
    for token in extraction.markup_signature(text):
        name = token.strip("</>")
        if token.endswith("/>") or name in VOID_MARKUP:
            continue
        if token.startswith("</"):
            if not stack or stack.pop() != name:
                raise VerificationError(f"{label}: markup is unbalanced at {token}")
        else:
            stack.append(name)
    if stack:
        raise VerificationError(f"{label}: markup is unbalanced; unclosed <{stack[-1]}>")
    for delimiter in ("**", "__", "`"):
        if text.count(delimiter) % 2:
            raise VerificationError(f"{label}: Markdown delimiter {delimiter!r} is unbalanced")


def accelerator_count(text: str) -> int:
    count = 0
    index = 0
    while index < len(text):
        if text[index] != "&":
            index += 1
            continue
        if index + 1 < len(text) and text[index + 1] == "&":
            index += 2
            continue
        count += 1
        index += 1
    return count


def translation_hash(forms: list[str]) -> str:
    payload = json.dumps(forms, ensure_ascii=False, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def validate_translation(label: str, message: dict[str, object], text: str) -> None:
    validate_text(label, text)
    validate_markup(label, text)
    if label.startswith("ko-KR/") and SEPARATED_KOREAN_PARTICLE.search(text):
        raise VerificationError(f"{label}: Korean particle is separated from protected name")
    source = str(message["source"])
    if Counter(extraction.PLACEHOLDER.findall(text)) != Counter(
        extraction.PLACEHOLDER.findall(source)
    ):
        raise VerificationError(f"{label}: placeholder multiset differs from English source")
    if extraction.markup_signature(text) != message["markup_signature"]:
        raise VerificationError(f"{label}: markup signature differs from English source")
    source_tokens = Counter(extraction.protected_tokens(source))
    translated_tokens = Counter(extraction.protected_tokens(text))
    unexpected = translated_tokens - source_tokens
    # A translation may name the product explicitly where the English source
    # uses an implied subject. Other added protected values (URLs, executables,
    # registry paths, IDs) remain forbidden, and source tokens remain exact.
    permitted_brand_context = (
        unexpected == Counter({"GameHQ": 1}) and source_tokens["GameHQ"] == 0
    )
    if any(translated_tokens[token] != count for token, count in source_tokens.items()) \
            or (unexpected and not permitted_brand_context):
        raise VerificationError(f"{label}: protected token differs from English source")
    for token in message["protected_tokens"]:
        if text.count(str(token)) != source.count(str(token)):
            raise VerificationError(
                f"{label}: protected literal {token!r} differs from English source"
            )
    if (text.count("\n"), text.count("\\n")) != (
        source.count("\n"),
        source.count("\\n"),
    ):
        raise VerificationError(f"{label}: line-break token count differs from English source")
    mnemonic_surface = any(
        not str(location.get("file", "")).endswith(".qml")
        for location in message.get("locations", [])
        if isinstance(location, dict)
    )
    if mnemonic_surface and not message["markup_signature"] \
            and accelerator_count(text) != accelerator_count(source):
        raise VerificationError(f"{label}: accelerator marker count differs from English source")


def load_manifest(path: Path) -> tuple[dict[str, object], dict[str, dict[str, object]]]:
    manifest = read_json(path)
    try:
        extraction.validate_manifest(manifest)
    except extraction.SyncError as error:
        raise VerificationError(f"manifest schema violation: {error}")
    messages: dict[str, dict[str, object]] = {}
    for value in manifest["messages"]:
        message = dict(value)
        message_id = str(message["id"])
        expected_hash = extraction.message_source_hash(
            str(message["source"]), str(message["context"])
        )
        if message["source_hash"] != expected_hash:
            raise VerificationError(f"{message_id}: source hash does not match source and context")
        if message["protected_tokens"] != extraction.protected_tokens(str(message["source"])):
            raise VerificationError(f"{message_id}: protected-token manifest is stale")
        if message["markup_signature"] != extraction.markup_signature(str(message["source"])):
            raise VerificationError(f"{message_id}: markup manifest is stale")
        expected_placeholders = sorted(set(extraction.PLACEHOLDER.findall(str(message["source"]))))
        if message["placeholders"] != expected_placeholders:
            raise VerificationError(f"{message_id}: placeholder manifest is stale")
        if message["domain"] != extraction.message_domain(message_id):
            raise VerificationError(f"{message_id}: domain classification is stale")
        plural_token = "%n" in extraction.PLACEHOLDER.findall(str(message["source"]))
        if bool(message["plural"]) != plural_token:
            raise VerificationError(f"{message_id}: plural flag and %n usage disagree")
        validate_text(f"{message_id} English source", str(message["source"]))
        validate_markup(f"{message_id} English source", str(message["source"]))
        messages[message_id] = message
    return manifest, messages


def load_registry(path: Path) -> tuple[str, list[dict[str, object]]]:
    registry = read_json(path)
    source_locale = registry.get("source_language")
    locales = registry.get("locales")
    if not isinstance(source_locale, str) or not isinstance(locales, list):
        raise VerificationError("locale registry must contain source_language and locales")
    seen_tags: set[str] = set()
    seen_catalogs: set[str] = set()
    normalized: list[dict[str, object]] = []
    for value in locales:
        if not isinstance(value, dict):
            raise VerificationError("locale registry entry must be an object")
        required = {"tag", "state", "fallback", "qt_catalog", "completeness_policy"}
        if not required.issubset(value):
            raise VerificationError("locale registry entry lacks verification fields")
        locale = dict(value)
        tag = str(locale["tag"])
        catalog = str(locale["qt_catalog"])
        if tag in seen_tags or catalog in seen_catalogs:
            raise VerificationError(f"duplicate locale tag or catalog mapping for {tag}")
        seen_tags.add(tag)
        seen_catalogs.add(catalog)
        normalized.append(locale)
    if source_locale not in seen_tags:
        raise VerificationError("source language is absent from the locale registry")
    return source_locale, sorted(normalized, key=lambda value: str(value["tag"]))


def element_text(element: ET.Element | None) -> str:
    return "" if element is None else "".join(element.itertext())


def expected_plural_forms(locale: str) -> int:
    language = normalized_tag(locale).split("-", 1)[0].lower()
    if language not in PLURAL_FORMS:
        raise VerificationError(f"plural-form rule is undefined for locale {locale}")
    return PLURAL_FORMS[language]


def read_catalog(
    path: Path,
    locale: str,
    messages: dict[str, dict[str, object]],
) -> dict[str, dict[str, object] | None]:
    try:
        text = path.read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise VerificationError(f"cannot read UTF-8 catalog {path}: {error}")
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        raise VerificationError(f"cannot parse TS catalog {path}: {error}")
    if root.tag != "TS" or normalized_tag(root.get("language", "")) != locale:
        raise VerificationError(f"catalog {path} does not declare locale {locale}")

    indexed: dict[str, ET.Element] = {}
    for element in root.findall("./context/message"):
        message_id = element.get("id", "")
        if not message_id:
            continue
        if not extraction.ID_PATTERN.fullmatch(message_id):
            raise VerificationError(f"catalog {path} contains invalid ID {message_id!r}")
        if message_id in indexed:
            raise VerificationError(f"catalog {path} contains duplicate ID {message_id!r}")
        indexed[message_id] = element

    for message_id, element in indexed.items():
        if message_id in messages:
            continue
        translation = element.find("translation")
        if translation is None or translation.get("type") not in {"vanished", "obsolete"}:
            raise VerificationError(f"catalog {path} contains active unknown ID {message_id!r}")

    result: dict[str, dict[str, object] | None] = {}
    for message_id, message in messages.items():
        element = indexed.get(message_id)
        if element is None:
            result[message_id] = None
            continue
        source = element.findtext("source", default="")
        if source != message["source"]:
            raise VerificationError(f"{locale}/{message_id}: TS source differs from extraction manifest")
        plural = element.get("numerus") == "yes"
        if plural != bool(message["plural"]):
            raise VerificationError(f"{locale}/{message_id}: TS plural flag differs from source")
        translation = element.find("translation")
        if translation is None:
            result[message_id] = None
            continue
        if translation.get("type") in {"vanished", "obsolete"}:
            raise VerificationError(f"{locale}/{message_id}: active translation is marked obsolete")
        unfinished = translation.get("type") == "unfinished"
        if plural:
            forms = [element_text(form) for form in translation.findall("numerusform")]
            if not forms:
                result[message_id] = None
                continue
            expected = expected_plural_forms(locale)
            if len(forms) != expected:
                raise VerificationError(
                    f"{locale}/{message_id}: expected {expected} plural forms, found {len(forms)}"
                )
        else:
            if translation.findall("numerusform"):
                raise VerificationError(f"{locale}/{message_id}: singular message has plural forms")
            forms = [element_text(translation)]

        for index, value in enumerate(forms):
            if value:
                validate_translation(f"{locale}/{message_id} form {index + 1}", message, value)
        if any(not value for value in forms):
            result[message_id] = None
            continue
        result[message_id] = {"forms": forms, "unfinished": unfinished}
    return result


def valid_timestamp(value: object) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return True


def validate_state_entry(label: str, entry: dict[str, object], source_locale: bool) -> None:
    required = {"domain", "source_hash", "translation_hash", "status", "provenance", "updated_at"}
    if set(entry) != required:
        raise VerificationError(f"{label}: state entry fields do not match schema")
    if entry["domain"] not in DOMAINS or not HASH_PATTERN.fullmatch(str(entry["source_hash"])):
        raise VerificationError(f"{label}: state domain or source hash is invalid")
    translation = entry["translation_hash"]
    if translation is not None and not HASH_PATTERN.fullmatch(str(translation)):
        raise VerificationError(f"{label}: translation hash is invalid")
    status = entry["status"]
    if status not in STATUSES:
        raise VerificationError(f"{label}: translation status is invalid")
    provenance = entry["provenance"]
    if not isinstance(provenance, dict) or set(provenance) != {"kind", "actor"}:
        raise VerificationError(f"{label}: provenance does not match schema")
    kind = provenance["kind"]
    actor = provenance["actor"]
    if kind not in PROVENANCE_KINDS or (actor is not None and not isinstance(actor, str)):
        raise VerificationError(f"{label}: provenance value is invalid")
    updated = entry["updated_at"]
    if updated is not None and not valid_timestamp(updated):
        raise VerificationError(f"{label}: updated_at is not an ISO-8601 timestamp")
    if status in {"missing", "intentionally_inherited"} and translation is not None:
        raise VerificationError(f"{label}: untranslated status cannot carry a translation hash")
    if status not in {"missing", "intentionally_inherited"} and translation is None:
        raise VerificationError(f"{label}: translated status requires a translation hash")
    if status.startswith("machine_") and (kind != "machine" or updated is None):
        raise VerificationError(f"{label}: machine state lacks machine provenance or timestamp")
    if status == "contextually_reviewed" and (kind != "agent" or updated is None):
        raise VerificationError(f"{label}: contextual state lacks agent provenance or timestamp")
    if status == "human_reviewed" and not source_locale and (kind != "human" or updated is None):
        raise VerificationError(f"{label}: reviewed state lacks human provenance or timestamp")


def validate_completeness(label: str, completeness: object) -> None:
    if not isinstance(completeness, dict) or set(completeness) != set(DOMAINS):
        raise VerificationError(f"{label}: completeness domains do not match schema")
    for domain, state in completeness.items():
        if not isinstance(state, dict) or set(state) != {"total", "current", "missing", "stale"}:
            raise VerificationError(f"{label}/{domain}: completeness fields do not match schema")
        if not isinstance(state["total"], int) or not isinstance(state["current"], int):
            raise VerificationError(f"{label}/{domain}: completeness counts are invalid")
        if state["total"] < 0 or not 0 <= state["current"] <= state["total"]:
            raise VerificationError(f"{label}/{domain}: completeness counts are inconsistent")
        for key in ("missing", "stale"):
            if not isinstance(state[key], list) or state[key] != sorted(set(state[key])):
                raise VerificationError(f"{label}/{domain}: {key} IDs are not uniquely sorted")
            if any(not extraction.ID_PATTERN.fullmatch(value) for value in state[key]):
                raise VerificationError(f"{label}/{domain}: {key} contains an invalid ID")


def load_state(path: Path, source_locale: str) -> dict[str, object]:
    if not path.is_file():
        return {"locales": {}}
    state = read_json(path)
    required = {"$schema", "schema_version", "source_manifest", "locales"}
    if set(state) != required or state["schema_version"] != 2 or not isinstance(state["locales"], dict):
        raise VerificationError("translation state root does not match schema version 2")
    for locale, value in state["locales"].items():
        if not isinstance(value, dict) or set(value) != {
            "catalog", "catalog_present", "enabled", "messages", "completeness"
        }:
            raise VerificationError(f"{locale}: locale state does not match schema")
        if not isinstance(value["catalog"], str) or not value["catalog"]:
            raise VerificationError(f"{locale}: catalog path is invalid")
        if type(value["catalog_present"]) is not bool or type(value["enabled"]) is not bool:
            raise VerificationError(f"{locale}: catalog/enabled state must be boolean")
        if not isinstance(value["messages"], dict):
            raise VerificationError(f"{locale}: messages state must be an object")
        for message_id, entry in value["messages"].items():
            if not extraction.ID_PATTERN.fullmatch(message_id) or not isinstance(entry, dict):
                raise VerificationError(f"{locale}: message state contains an invalid ID")
            validate_state_entry(f"{locale}/{message_id}", entry, locale == source_locale)
        validate_completeness(locale, value["completeness"])
    return state


def missing_entry(message: dict[str, object]) -> dict[str, object]:
    return {
        "domain": message["domain"],
        "source_hash": message["source_hash"],
        "translation_hash": None,
        "status": "missing",
        "provenance": {"kind": "none", "actor": None},
        "updated_at": None,
    }


def source_entry(message: dict[str, object], payload: dict[str, object]) -> dict[str, object]:
    return {
        "domain": message["domain"],
        "source_hash": message["source_hash"],
        "translation_hash": translation_hash(payload["forms"]),
        "status": "human_reviewed",
        "provenance": {"kind": "source", "actor": "repository"},
        "updated_at": None,
    }


def stale_entry(message: dict[str, object], payload_hash: str) -> dict[str, object]:
    return {
        "domain": message["domain"],
        "source_hash": message["source_hash"],
        "translation_hash": payload_hash,
        "status": "stale",
        "provenance": {"kind": "unknown", "actor": None},
        "updated_at": None,
    }


def completeness_for(
    messages: dict[str, dict[str, object]], state: dict[str, dict[str, object]]
) -> dict[str, object]:
    result: dict[str, object] = {}
    for domain in DOMAINS:
        ids = sorted(
            message_id for message_id, message in messages.items()
            if message["domain"] == domain
        )
        missing = [message_id for message_id in ids if state[message_id]["status"] == "missing"]
        stale = [message_id for message_id in ids if state[message_id]["status"] == "stale"]
        current = sum(1 for message_id in ids if state[message_id]["status"] in CURRENT_STATUSES)
        result[domain] = {
            "total": len(ids),
            "current": current,
            "missing": missing,
            "stale": stale,
        }
    return result


def reconcile(
    root: Path,
    manifest_path: Path,
    registry_path: Path,
    state_path: Path,
) -> dict[str, object]:
    _, messages = load_manifest(manifest_path)
    source_locale, locales = load_registry(registry_path)
    previous = load_state(state_path, source_locale)
    previous_locales = previous.get("locales", {})
    catalog_dir = root / "i18n" / "app"
    expected_catalogs = {f"{locale['qt_catalog']}.ts" for locale in locales}
    for catalog in catalog_dir.glob("*.ts"):
        if catalog.name not in expected_catalogs:
            raise VerificationError(f"catalog is not registered: {catalog.relative_to(root).as_posix()}")

    locale_states: dict[str, object] = {}
    for locale in locales:
        tag = str(locale["tag"])
        catalog_path = catalog_dir / f"{locale['qt_catalog']}.ts"
        present = catalog_path.is_file()
        if tag == source_locale and not present:
            raise VerificationError("source-locale catalog is missing")
        payloads = (
            read_catalog(catalog_path, tag, messages)
            if present else {message_id: None for message_id in messages}
        )
        old_locale = previous_locales.get(tag, {}) if isinstance(previous_locales, dict) else {}
        old_messages = old_locale.get("messages", {}) if isinstance(old_locale, dict) else {}
        reconciled: dict[str, dict[str, object]] = {}
        for message_id, message in messages.items():
            payload = payloads[message_id]
            old = old_messages.get(message_id) if isinstance(old_messages, dict) else None
            if tag == source_locale:
                if payload is None:
                    raise VerificationError(f"{tag}/{message_id}: source translation is missing")
                if payload["unfinished"]:
                    raise VerificationError(f"{tag}/{message_id}: source translation is unfinished")
                reconciled[message_id] = source_entry(message, payload)
                continue
            if payload is None:
                if isinstance(old, dict) and old.get("status") == "intentionally_inherited":
                    permitted = locale.get("fallback") and locale.get("state") != "enabled"
                    if not permitted:
                        raise VerificationError(
                            f"{tag}/{message_id}: intentionally inherited state is not permitted"
                        )
                    inherited = dict(old)
                    inherited["domain"] = message["domain"]
                    inherited["source_hash"] = message["source_hash"]
                    reconciled[message_id] = inherited
                else:
                    reconciled[message_id] = missing_entry(message)
                continue

            actual_hash = translation_hash(payload["forms"])
            if not isinstance(old, dict) or old.get("status") in {
                "missing", "intentionally_inherited"
            }:
                reconciled[message_id] = stale_entry(message, actual_hash)
                continue
            drifted = (
                old.get("source_hash") != message["source_hash"]
                or old.get("translation_hash") != actual_hash
                or bool(payload["unfinished"])
            )
            if drifted:
                stale = dict(old)
                stale["domain"] = message["domain"]
                stale["status"] = "stale"
                reconciled[message_id] = stale
            else:
                current = dict(old)
                current["domain"] = message["domain"]
                reconciled[message_id] = current

        locale_states[tag] = {
            "catalog": catalog_path.relative_to(root).as_posix(),
            "catalog_present": present,
            "enabled": locale.get("state") == "enabled",
            "messages": {key: reconciled[key] for key in sorted(reconciled)},
            "completeness": completeness_for(messages, reconciled),
        }

    return {
        "$schema": "../schema/message-state.schema.json",
        "schema_version": 2,
        "source_manifest": manifest_path.relative_to(root).as_posix(),
        "locales": {key: locale_states[key] for key in sorted(locale_states)},
    }


def release_failures(state: dict[str, object]) -> list[str]:
    failures: list[str] = []
    for locale, value in state["locales"].items():
        if not value["enabled"]:
            continue
        for domain, coverage in value["completeness"].items():
            if coverage["current"] != coverage["total"]:
                failures.append(
                    f"enabled locale {locale}/{domain} is incomplete "
                    f"({coverage['current']}/{coverage['total']})"
                )
    return failures


def verify(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    manifest_path = (root / args.manifest).resolve()
    registry_path = (root / args.registry).resolve()
    state_path = (root / args.state).resolve()
    desired = reconcile(root, manifest_path, registry_path, state_path)
    content = json_bytes(desired)
    divergent = not state_path.is_file() or state_path.read_bytes() != content
    if divergent and not args.update_state:
        print(
            f"translation state requires update: {state_path.relative_to(root).as_posix()}",
            file=sys.stderr,
        )
        return 1
    if divergent:
        extraction.atomic_write(state_path, content)
        print(f"updated {state_path.relative_to(root).as_posix()}")
    else:
        print("translation state is byte-current")

    for locale, value in desired["locales"].items():
        coverage = value["completeness"]["application"]
        print(f"{locale}: application={coverage['current']}/{coverage['total']}")
    if args.release:
        failures = release_failures(desired)
        if failures:
            for failure in failures:
                print(f"release gate: {failure}", file=sys.stderr)
            return 1
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--root", required=True, help="project root")
    result.add_argument("--manifest", default="i18n/extracted/messages.json")
    result.add_argument("--registry", default="i18n/locales.json")
    result.add_argument("--state", default="i18n/state/translations.json")
    result.add_argument("--update-state", action="store_true")
    result.add_argument("--release", action="store_true")
    return result


def main() -> int:
    try:
        return verify(parser().parse_args())
    except (VerificationError, extraction.SyncError) as error:
        print(f"i18n verification error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
