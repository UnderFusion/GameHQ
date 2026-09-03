#!/usr/bin/env python3
"""Validate localized GameHQ-owned Inno Setup CustomMessages."""

from __future__ import annotations

import copy
import importlib.util
import json
import re
import sys
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_LOCALES = {
    "en-US", "zh-Hans", "ru-RU", "es-ES", "pt-BR", "de-DE", "ja-JP",
    "fr-FR", "pl-PL", "ko-KR", "zh-Hant", "tr-TR", "th-TH", "es-419",
    "uk-UA", "it-IT",
}
PSEUDO_LOCALES = {"en-XA", "ar-XB"}
PLACEHOLDER = re.compile(r"\[name/ver\]|%[1-9n]")
KEY = re.compile(r"GameHQ[A-Za-z0-9]+\Z")


class ValidationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ValidationError(message)


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result: dict = {}
    for key, value in pairs:
        if key in result:
            fail(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)


def production_locales(locale_manifest: dict) -> list[dict]:
    locales = [
        locale
        for locale in locale_manifest["locales"]
        if locale.get("state") == "enabled" and locale.get("tier") == 1
    ]
    tags = {locale["tag"] for locale in locales}
    if tags != EXPECTED_LOCALES or len(locales) != 16:
        fail("CustomMessages must use exactly the sixteen production locales")
    if tags & PSEUDO_LOCALES:
        fail("pseudo-locales must not become production installer messages")
    for locale in locales:
        if not locale.get("inno_language"):
            fail(f"{locale['tag']} has no Inno language name")
        if not (ROOT / "i18n" / "style" / f"{locale['tag']}.json").is_file():
            fail(f"{locale['tag']} has no locale style guide")
    return sorted(locales, key=lambda locale: locale["inno_order"])


def validate_message_data(message_manifest: dict, locales: list[dict]) -> None:
    if message_manifest.get("schema_version") != 1:
        fail("unsupported CustomMessages schema version")
    if message_manifest.get("source_locale") != "en-US":
        fail("CustomMessages source locale must be en-US")
    if message_manifest.get("fallback_locale") != "en-US":
        fail("CustomMessages fallback locale must be en-US")
    quality = message_manifest.get("quality_reference", {})
    if quality.get("locale") != "pl-PL" or "word" not in quality.get("instruction", ""):
        fail("Polish contextual quality reference is missing")

    translations = message_manifest.get("translations", {})
    expected_tags = {locale["tag"] for locale in locales}
    if set(translations) != expected_tags:
        fail("translation locale set differs from the sixteen production locales")
    if set(translations) & PSEUDO_LOCALES:
        fail("pseudo-locales must not have production CustomMessages")

    messages = message_manifest.get("messages", [])
    keys = [message.get("key", "") for message in messages]
    folded = [key.casefold() for key in keys]
    if len(keys) != len(set(folded)):
        fail("duplicate CustomMessages key")
    if not keys or any(not KEY.fullmatch(key) for key in keys):
        fail("invalid or empty CustomMessages key")

    expected_keys = set(keys)
    for tag, localized in translations.items():
        if set(localized) != expected_keys:
            fail(f"{tag} has missing or extra CustomMessages keys")

    for message in messages:
        key = message["key"]
        source = message.get("source")
        if not isinstance(source, str) or not source:
            fail(f"{key} has no English source")
        if not message.get("context") or not message.get("surface_type"):
            fail(f"{key} lacks UI context or surface type")
        protected = message.get("protected_tokens")
        if not isinstance(protected, list):
            fail(f"{key} lacks protected-token metadata")
        for token in protected:
            if token not in source:
                fail(f"{key} protects a token absent from its English source: {token}")
        if translations["en-US"][key] != source:
            fail(f"{key} English fallback differs from its source")

        source_placeholders = Counter(PLACEHOLDER.findall(source))
        for locale in locales:
            tag = locale["tag"]
            text = translations[tag][key]
            if not isinstance(text, str) or not text.strip():
                fail(f"{tag}.{key} is empty")
            if "\n" in text or "\r" in text:
                fail(f"{tag}.{key} contains a physical line break")
            if "{" in text or "}" in text:
                fail(f"{tag}.{key} contains an unsafe Inno constant delimiter")
            if Counter(PLACEHOLDER.findall(text)) != source_placeholders:
                fail(f"{tag}.{key} placeholder set differs from English")
            for token in protected:
                if text.count(token) != source.count(token):
                    fail(f"{tag}.{key} protected token differs: {token}")
            if tag != "en-US" and text == source:
                fail(f"{tag}.{key} remains unexplained English")

    consumers = message_manifest.get("required_consumers", [])
    consumer_keys = [consumer.get("key") for consumer in consumers]
    if set(consumer_keys) != expected_keys or len(consumer_keys) != len(expected_keys):
        fail("owned message consumers do not cover every CustomMessages key exactly once")

    classifications = message_manifest.get("classified_nonlocalized", [])
    if not classifications:
        fail("English-by-policy installer literals are not classified")
    for entry in classifications:
        if entry.get("classification") != "English-by-policy" or not entry.get("rationale"):
            fail("an installer nonlocalized literal lacks classification or rationale")


def load_generator():
    path = ROOT / "tools" / "i18n" / "generate_inno_custom_messages.py"
    spec = importlib.util.spec_from_file_location("gamehq_inno_custom_generator", path)
    if spec is None or spec.loader is None:
        fail("cannot load Inno CustomMessages generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_repository(message_manifest: dict, locale_manifest: dict, locales: list[dict]) -> None:
    script = (ROOT / "packaging" / "GameHQ.iss").read_text(encoding="utf-8")
    include = '#include "generated\\InnoCustomMessages.iss"'
    if include not in script:
        fail("GameHQ.iss does not include generated CustomMessages")
    if "ActiveLanguage" in script:
        fail("GameHQ.iss contains forbidden locale branching")
    for consumer in message_manifest["required_consumers"]:
        if script.count(consumer["needle"]) != 1:
            fail(f"{consumer['location']} does not consume {consumer['key']} exactly once")
    for entry in message_manifest["classified_nonlocalized"]:
        if entry["literal"] not in script:
            fail(f"classified installer literal is stale: {entry['literal']}")
    for entry in message_manifest["protected_identity"]:
        if entry["literal"] not in script:
            fail(f"protected installer identity is stale: {entry['literal']}")

    forbidden = (
        "WelcomeLabel1=Welcome to GameHQ",
        "WelcomeLabel2=Setup will install",
        'Description: "Create a desktop shortcut"',
        'GroupDescription: "Additional shortcuts:"',
        'Description: "Launch GameHQ"',
        "Result := 'A GameHQ update is running'",
        "Result := 'A previous GameHQ update did not finish'",
        "MsgBox('GameHQ is running.",
    )
    for fragment in forbidden:
        if fragment in script:
            fail(f"hardcoded owned installer text remains: {fragment}")
    if re.search(r"MsgBox\(\s*'", script):
        fail("a Pascal MsgBox still starts with a hardcoded string")

    generator = load_generator()
    generated_path = ROOT / "packaging" / "generated" / "InnoCustomMessages.iss"
    generated = generated_path.read_text(encoding="utf-8") if generated_path.exists() else ""
    if generated != generator.render(locale_manifest, message_manifest):
        fail("generated Inno CustomMessages are stale")

    actual_keys: list[str] = []
    for raw in generated.splitlines():
        line = raw.strip()
        if not line or line.startswith(";") or line == "[CustomMessages]":
            continue
        if "=" not in line:
            fail(f"invalid generated CustomMessages line: {line}")
        actual_keys.append(line.split("=", 1)[0])
    expected_keys = []
    message_keys = [message["key"] for message in message_manifest["messages"]]
    for locale in locales:
        prefix = "" if locale["tag"] == "en-US" else f"{locale['inno_language']}."
        expected_keys.extend(f"{prefix}{key}" for key in message_keys)
    if actual_keys != expected_keys:
        fail("generated CustomMessages keys or locale ordering differ from the canonical manifests")
    if len({key.casefold() for key in actual_keys}) != len(actual_keys):
        fail("generated CustomMessages contain duplicate keys")

    build_script = (ROOT / "packaging" / "build-setup.ps1").read_text(encoding="utf-8")
    for marker in (
        "generate_inno_custom_messages.py",
        "test_inno_custom_messages.py",
        "InnoCustomMessages.iss",
    ):
        if marker not in build_script:
            fail(f"installer build does not enforce {marker}")


def expect_mutation_failure(
    base: dict,
    locales: list[dict],
    label: str,
    mutation,
    expected: str,
) -> None:
    candidate = copy.deepcopy(base)
    mutation(candidate)
    try:
        validate_message_data(candidate, locales)
    except ValidationError as error:
        if expected not in str(error):
            fail(f"{label} failed for the wrong reason: {error}")
        return
    fail(f"{label} mutation was not rejected")


def validate_negative_guards(message_manifest: dict, locales: list[dict]) -> int:
    guards = (
        (
            "missing locale key",
            lambda data: data["translations"]["pl-PL"].pop("GameHQLaunch"),
            "missing or extra",
        ),
        (
            "placeholder change",
            lambda data: data["translations"]["pl-PL"].__setitem__(
                "GameHQUpdateActive",
                data["translations"]["pl-PL"]["GameHQUpdateActive"].replace("%1", "%2"),
            ),
            "placeholder set",
        ),
        (
            "protected token change",
            lambda data: data["translations"]["pl-PL"].__setitem__(
                "GameHQLaunch", "Uruchom Game HQ"
            ),
            "protected token",
        ),
        (
            "duplicate message key",
            lambda data: data["messages"].append(copy.deepcopy(data["messages"][0])),
            "duplicate CustomMessages key",
        ),
        (
            "pseudo-locale addition",
            lambda data: data["translations"].__setitem__(
                "en-XA", copy.deepcopy(data["translations"]["en-US"])
            ),
            "translation locale set",
        ),
    )
    for label, mutation, expected in guards:
        expect_mutation_failure(message_manifest, locales, label, mutation, expected)
    return len(guards)


def main() -> int:
    locale_manifest = load_json(ROOT / "i18n" / "locales.json")
    message_manifest = load_json(ROOT / "packaging" / "i18n" / "custom-messages.json")
    locales = production_locales(locale_manifest)
    validate_message_data(message_manifest, locales)
    validate_repository(message_manifest, locale_manifest, locales)
    guards = validate_negative_guards(message_manifest, locales)
    print(
        f"Inno CustomMessages audit passed "
        f"({len(message_manifest['messages'])} messages, {len(locales)} locales, "
        f"{guards} mutation guards)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValidationError, json.JSONDecodeError, KeyError, TypeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
