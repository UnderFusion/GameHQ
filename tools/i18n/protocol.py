#!/usr/bin/env python3
"""Validate deterministic, bounded GameHQ translation-agent exchanges."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import sync as extraction
import verify


PROTOCOL_VERSION = 1
QUEUE_FIELDS = {
    "$schema", "protocol_version", "batch_id", "source_locale", "target_locale",
    "locale", "glossary_version", "style_version", "units",
}
QUEUE_UNIT_FIELDS = {
    "id", "source", "source_hash", "context", "location_hint", "domain", "plural",
    "placeholders", "markup_signature", "accelerator_count", "protected_tokens",
    "prior_translation", "prior_status",
}
RESPONSE_FIELDS = {
    "$schema", "protocol_version", "batch_id", "source_locale", "target_locale", "units",
}
RESPONSE_UNIT_FIELDS = {
    "id", "source_hash", "translation", "translation_hash", "status", "provenance",
}
PRIOR_STATUSES = {
    "missing", "machine_translated", "machine_verified", "contextually_reviewed",
    "human_reviewed", "stale",
    "intentionally_inherited",
}


class ProtocolError(RuntimeError):
    pass


def canonical_json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ProtocolError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def read_json(path: Path) -> dict[str, object]:
    try:
        text = path.read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise ProtocolError(f"cannot read UTF-8 JSON {path}: {error}")
    try:
        value = json.loads(text, object_pairs_hook=reject_duplicate_keys)
    except json.JSONDecodeError as error:
        raise ProtocolError(f"cannot parse JSON {path}: {error}")
    if not isinstance(value, dict):
        raise ProtocolError(f"JSON root must be an object: {path}")
    return value


def require_fields(label: str, value: dict[str, object], expected: set[str]) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ProtocolError(f"{label}: fields differ; missing={missing}, extra={extra}")


def require_string(label: str, value: object) -> str:
    if not isinstance(value, str) or not value:
        raise ProtocolError(f"{label}: expected a non-empty string")
    verify.validate_text(label, value)
    return value


def load_locale(root: Path, target_locale: str) -> dict[str, object]:
    registry = read_json(root / "i18n" / "locales.json")
    if registry.get("source_language") != "en-US":
        raise ProtocolError("locale registry source_language must be en-US")
    locales = registry.get("locales")
    if not isinstance(locales, list):
        raise ProtocolError("locale registry locales must be an array")
    for value in locales:
        if isinstance(value, dict) and value.get("tag") == target_locale:
            if value.get("tier") != 1 or value.get("state") != "enabled":
                raise ProtocolError(f"target_locale {target_locale} is not an enabled launch locale")
            return value
    raise ProtocolError(f"target_locale {target_locale} is absent from the locale registry")


def load_policy(root: Path, target_locale: str) -> tuple[dict[str, object], dict[str, object]]:
    glossary = read_json(root / "i18n" / "glossary" / "glossary.json")
    style = read_json(root / "i18n" / "style" / f"{target_locale}.json")
    require_fields("glossary", glossary, {
        "$schema", "schema_version", "source_language", "protected_literals",
        "protected_categories", "terminology",
    })
    require_fields("style", style, {
        "$schema", "schema_version", "locale", "source_language", "tone",
        "capitalization", "button_labels", "ui_roles", "technical_terms",
        "contextual_terminology", "punctuation", "units", "natural_language",
    })
    if glossary.get("schema_version") != 2 or glossary.get("source_language") != "en-US":
        raise ProtocolError("glossary: unsupported version or source language")
    if style.get("schema_version") != 2 or style.get("source_language") != "en-US":
        raise ProtocolError("style: unsupported version or source language")
    if style.get("locale") != target_locale:
        raise ProtocolError(f"style: locale does not match target_locale {target_locale}")
    for field in (
        "tone", "capitalization", "button_labels", "ui_roles", "technical_terms",
        "contextual_terminology", "punctuation", "units", "natural_language",
    ):
        require_string(f"style.{field}", style.get(field))
    literals = glossary.get("protected_literals")
    if not isinstance(literals, list) or not literals:
        raise ProtocolError("glossary.protected_literals must be a non-empty array")
    for index, literal in enumerate(literals):
        if not isinstance(literal, dict):
            raise ProtocolError(f"glossary.protected_literals[{index}] must be an object")
        require_fields(f"glossary.protected_literals[{index}]", literal, {"term", "reason"})
        require_string(f"glossary.protected_literals[{index}].term", literal.get("term"))
        require_string(f"glossary.protected_literals[{index}].reason", literal.get("reason"))
    categories = glossary.get("protected_categories")
    if not isinstance(categories, list) or not categories:
        raise ProtocolError("glossary.protected_categories must be a non-empty array")
    for index, category in enumerate(categories):
        require_string(f"glossary.protected_categories[{index}]", category)
    terminology = glossary.get("terminology")
    if not isinstance(terminology, list) or not terminology:
        raise ProtocolError("glossary.terminology must be a non-empty array")
    for index, term in enumerate(terminology):
        if not isinstance(term, dict):
            raise ProtocolError(f"glossary.terminology[{index}] must be an object")
        require_fields(
            f"glossary.terminology[{index}]", term,
            {"source", "meaning", "instruction", "contexts"},
        )
        for field in ("source", "meaning", "instruction"):
            require_string(f"glossary.terminology[{index}].{field}", term.get(field))
        contexts = term.get("contexts")
        if not isinstance(contexts, list) or not contexts:
            raise ProtocolError(f"glossary.terminology[{index}].contexts must be a non-empty array")
        for context_index, context in enumerate(contexts):
            label = f"glossary.terminology[{index}].contexts[{context_index}]"
            if not isinstance(context, dict):
                raise ProtocolError(f"{label} must be an object")
            require_fields(label, context, {"when", "meaning", "instruction"})
            for field in ("when", "meaning", "instruction"):
                require_string(f"{label}.{field}", context.get(field))
    return glossary, style


def validate_queue(root: Path, queue: dict[str, object]) -> dict[str, dict[str, object]]:
    require_fields("queue", queue, QUEUE_FIELDS)
    if queue.get("protocol_version") != PROTOCOL_VERSION:
        raise ProtocolError("queue.protocol_version: unsupported version")
    if queue.get("source_locale") != "en-US":
        raise ProtocolError("queue.source_locale: en-US is the only authoritative source")
    target_locale = require_string("queue.target_locale", queue.get("target_locale"))
    if target_locale == "en-US":
        raise ProtocolError("queue.target_locale: source locale cannot be translated")
    locale = load_locale(root, target_locale)
    glossary, style = load_policy(root, target_locale)
    if queue.get("glossary_version") != glossary.get("schema_version"):
        raise ProtocolError("queue.glossary_version: does not match the loaded glossary")
    if queue.get("style_version") != style.get("schema_version"):
        raise ProtocolError("queue.style_version: does not match the loaded style guide")
    locale_input = queue.get("locale")
    if not isinstance(locale_input, dict):
        raise ProtocolError("queue.locale: expected an object")
    require_fields("queue.locale", locale_input, {"direction", "fallback"})
    if locale_input != {"direction": locale.get("direction"), "fallback": locale.get("fallback")}:
        raise ProtocolError("queue.locale: metadata differs from i18n/locales.json")
    require_string("queue.batch_id", queue.get("batch_id"))
    units = queue.get("units")
    if not isinstance(units, list) or not units:
        raise ProtocolError("queue.units: expected a non-empty array")
    by_id: dict[str, dict[str, object]] = {}
    glossary_terms = [str(value["term"]) for value in glossary["protected_literals"]]
    for index, value in enumerate(units):
        label = f"queue.units[{index}]"
        if not isinstance(value, dict):
            raise ProtocolError(f"{label}: expected an object")
        require_fields(label, value, QUEUE_UNIT_FIELDS)
        message_id = require_string(f"{label}.id", value.get("id"))
        if not extraction.ID_PATTERN.fullmatch(message_id):
            raise ProtocolError(f"{label}.id: invalid message ID")
        if message_id in by_id:
            raise ProtocolError(f"{label}.id: duplicate ID {message_id}")
        source = require_string(f"{label}.source", value.get("source"))
        context = require_string(f"{label}.context", value.get("context"))
        require_string(f"{label}.location_hint", value.get("location_hint"))
        expected_hash = extraction.message_source_hash(source, context)
        if value.get("source_hash") != expected_hash:
            raise ProtocolError(f"{label}.source_hash: stale or inconsistent English source")
        plural = value.get("plural")
        if not isinstance(plural, bool) or plural != ("%n" in extraction.PLACEHOLDER.findall(source)):
            raise ProtocolError(f"{label}.plural: disagrees with English %n usage")
        expected_placeholders = sorted(set(extraction.PLACEHOLDER.findall(source)))
        if value.get("placeholders") != expected_placeholders:
            raise ProtocolError(f"{label}.placeholders: differs from English source")
        if value.get("markup_signature") != extraction.markup_signature(source):
            raise ProtocolError(f"{label}.markup_signature: differs from English source")
        expected_tokens = sorted(set(extraction.protected_tokens(source)) | {term for term in glossary_terms if term in source})
        if value.get("protected_tokens") != expected_tokens:
            raise ProtocolError(f"{label}.protected_tokens: differs from English source and glossary")
        if value.get("accelerator_count") != verify.accelerator_count(source):
            raise ProtocolError(f"{label}.accelerator_count: differs from English source")
        if value.get("domain") not in verify.DOMAINS:
            raise ProtocolError(f"{label}.domain: invalid domain")
        prior_status = value.get("prior_status")
        prior_translation = value.get("prior_translation")
        if prior_status not in PRIOR_STATUSES:
            raise ProtocolError(f"{label}.prior_status: invalid state")
        if prior_translation is not None and (not isinstance(prior_translation, list) or not prior_translation or not all(isinstance(form, str) and form for form in prior_translation)):
            raise ProtocolError(f"{label}.prior_translation: expected null or non-empty string array")
        if prior_status == "stale" and prior_translation is None:
            raise ProtocolError(f"{label}.prior_translation: stale units must include prior translation")
        verify.validate_text(f"{label}.source", source)
        verify.validate_markup(f"{label}.source", source)
        by_id[message_id] = value
    if list(by_id) != sorted(by_id):
        raise ProtocolError("queue.units: IDs must be sorted for deterministic serialization")
    return by_id


def validate_response(queue: dict[str, object], queued: dict[str, dict[str, object]], response: dict[str, object]) -> None:
    require_fields("response", response, RESPONSE_FIELDS)
    for field in ("protocol_version", "batch_id", "source_locale", "target_locale"):
        if response.get(field) != queue.get(field):
            raise ProtocolError(f"response.{field}: does not match queue")
    units = response.get("units")
    if not isinstance(units, list) or not units:
        raise ProtocolError("response.units: expected a non-empty array")
    seen: set[str] = set()
    target_locale = str(queue["target_locale"])
    for index, value in enumerate(units):
        label = f"response.units[{index}]"
        if not isinstance(value, dict):
            raise ProtocolError(f"{label}: expected an object")
        require_fields(label, value, RESPONSE_UNIT_FIELDS)
        message_id = require_string(f"{label}.id", value.get("id"))
        if message_id not in queued:
            raise ProtocolError(f"{label}.id: unrequested or wrong ID {message_id}")
        if message_id in seen:
            raise ProtocolError(f"{label}.id: duplicate ID {message_id}")
        seen.add(message_id)
        message = queued[message_id]
        if value.get("source_hash") != message.get("source_hash"):
            raise ProtocolError(f"{label}.source_hash: stale source hash")
        translations = value.get("translation")
        if not isinstance(translations, list) or not translations or not all(isinstance(form, str) and form for form in translations):
            raise ProtocolError(f"{label}.translation: expected a non-empty string array")
        expected_forms = verify.expected_plural_forms(target_locale) if message["plural"] else 1
        if len(translations) != expected_forms:
            raise ProtocolError(f"{label}.translation: expected {expected_forms} plural forms")
        for form_index, text in enumerate(translations):
            verify.validate_translation(f"{label}.translation[{form_index}]", message, text)
            if bool(message["plural"]) != ("%n" in extraction.PLACEHOLDER.findall(text)):
                raise ProtocolError(f"{label}.translation[{form_index}]: plural %n usage differs")
        expected_translation_hash = verify.translation_hash(translations)
        if value.get("translation_hash") != expected_translation_hash:
            raise ProtocolError(f"{label}.translation_hash: does not match translation payload")
        if value.get("status") != "machine_translated":
            raise ProtocolError(f"{label}.status: agent output must be machine_translated")
        provenance = value.get("provenance")
        if not isinstance(provenance, dict):
            raise ProtocolError(f"{label}.provenance: expected an object")
        require_fields(f"{label}.provenance", provenance, {"kind", "actor"})
        if provenance.get("kind") != "machine":
            raise ProtocolError(f"{label}.provenance.kind: must be machine")
        require_string(f"{label}.provenance.actor", provenance.get("actor"))
    if seen != set(queued):
        raise ProtocolError(f"response.units: ID set differs from queue; missing={sorted(set(queued) - seen)}")
    if [str(value["id"]) for value in units] != sorted(seen):
        raise ProtocolError("response.units: IDs must be sorted for deterministic serialization")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--queue", type=Path, required=True)
    parser.add_argument("--response", type=Path, required=True)
    parser.add_argument("--canonical-response", type=Path)
    arguments = parser.parse_args()
    try:
        queue = read_json(arguments.queue)
        response = read_json(arguments.response)
        queued = validate_queue(arguments.root.resolve(), queue)
        validate_response(queue, queued, response)
        if arguments.canonical_response:
            extraction.atomic_write(arguments.canonical_response, canonical_json_bytes(response))
        print(f"validated {len(queued)} translation unit(s) for {queue['target_locale']}")
        return 0
    except (ProtocolError, verify.VerificationError, extraction.SyncError) as error:
        print(f"translation protocol validation failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
