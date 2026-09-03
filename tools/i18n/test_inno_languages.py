#!/usr/bin/env python3
"""Validate GameHQ's manifest-driven Inno Setup language mapping."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_IDS = {
    "en-US": "$0409",
    "zh-Hans": "$0804",
    "ru-RU": "$0419",
    "es-ES": "$0C0A",
    "pt-BR": "$0416",
    "de-DE": "$0407",
    "ja-JP": "$0411",
    "fr-FR": "$040C",
    "pl-PL": "$0415",
    "ko-KR": "$0412",
    "zh-Hant": "$0404",
    "tr-TR": "$041F",
    "th-TH": "$041E",
    "es-419": "$080A",
    "uk-UA": "$0422",
    "it-IT": "$0410",
}
REQUIRED_FIELDS = (
    "inno_language",
    "inno_message_file",
    "inno_language_name",
    "inno_language_id",
    "inno_app_locale",
    "inno_order",
    "inno_source_kind",
    "inno_source_locale",
    "inno_fallback_locale",
)


def fail(message: str) -> None:
    raise AssertionError(message)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_psd1_value(text: str, name: str) -> str:
    match = re.search(rf"^\s*{re.escape(name)}\s*=\s*'([^']+)'", text, re.MULTILINE)
    if not match:
        fail(f"missing {name} in Inno toolchain manifest")
    return match.group(1)


def load_generator():
    path = ROOT / "tools" / "i18n" / "generate_inno_languages.py"
    spec = importlib.util.spec_from_file_location("gamehq_inno_generator", path)
    if spec is None or spec.loader is None:
        fail("cannot load Inno language generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def compiler_path(root: Path, source: str) -> Path:
    relative = source.removeprefix("compiler:").replace("\\", "/")
    return root / relative


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler-root", type=Path)
    parser.add_argument("--require-compiler", action="store_true")
    args = parser.parse_args()

    locale_manifest = json.loads((ROOT / "i18n" / "locales.json").read_text(encoding="utf-8"))
    provenance = json.loads(
        (ROOT / "packaging" / "inno" / "languages" / "provenance.json").read_text(encoding="utf-8")
    )
    locales = {
        locale["tag"]: locale
        for locale in locale_manifest["locales"]
        if locale.get("state") == "enabled" and locale.get("tier") == 1
    }
    if set(locales) != set(EXPECTED_IDS) or len(locales) != 16:
        fail("installer mapping must cover exactly the sixteen production locales")
    if {"en-XA", "ar-XB"} & set(locales):
        fail("pseudo-locales must not become installer languages")

    for tag, locale in locales.items():
        missing = [field for field in REQUIRED_FIELDS if field not in locale]
        if missing:
            fail(f"{tag} lacks explicit installer fields: {', '.join(missing)}")
        if locale["inno_language_id"].upper() != EXPECTED_IDS[tag]:
            fail(f"{tag} has incorrect LanguageID {locale['inno_language_id']}")
        if locale["inno_app_locale"] != tag:
            fail(f"{tag} changes the application handoff locale")
        for alias in locale.get("aliases", []):
            if locale_manifest["aliases"].get(alias) != tag:
                fail(f"{tag} alias {alias} is not canonical")
        source_locale = locale["inno_source_locale"]
        if source_locale not in locales:
            fail(f"{tag} has an unavailable installer UI source locale {source_locale}")
        fallback = locale["inno_fallback_locale"]
        if fallback is not None and fallback not in locales:
            fail(f"{tag} has an unavailable installer fallback locale {fallback}")
        if not locale["inno_message_file"].startswith("compiler:Default.isl"):
            fail(f"{tag} does not retain complete English fallback")

    for field in ("inno_language", "inno_language_id", "inno_order"):
        values = [locale[field] for locale in locales.values()]
        if len(values) != len(set(values)):
            fail(f"duplicate or ambiguous {field}")
    if {locale["inno_order"] for locale in locales.values()} != set(range(16)):
        fail("installer language order must be a complete 0..15 sequence")

    if locales["zh-Hans"]["inno_message_file"] == locales["zh-Hant"]["inno_message_file"]:
        fail("Simplified and Traditional Chinese must use distinct resources")
    if "BrazilianPortuguese.isl" not in locales["pt-BR"]["inno_message_file"]:
        fail("Brazilian Portuguese must not use Portugal Portuguese")
    latin = locales["es-419"]
    if latin["inno_source_kind"] != "source-locale-fallback" or latin["inno_source_locale"] != "es-ES":
        fail("Latin American Spanish must record its approved Spanish source fallback")
    if latin["inno_order"] >= locales["es-ES"]["inno_order"]:
        fail("Latin American Spanish must precede Spain for non-exact Spanish detection")

    generator = load_generator()
    generated_path = ROOT / "packaging" / "generated" / "InnoLanguages.iss"
    generated = generated_path.read_text(encoding="utf-8")
    if generated != generator.render(locale_manifest):
        fail("generated Inno language sections are stale")
    if generated.count('Name: "') != 16:
        fail("generated [Languages] section does not contain sixteen entries")

    script = (ROOT / "packaging" / "GameHQ.iss").read_text(encoding="utf-8")
    if '#include "generated\\InnoLanguages.iss"' not in script:
        fail("GameHQ.iss does not consume the generated language mapping")
    if "WelcomeLabel1=Welcome to GameHQ" not in script:
        fail("p5-1 must not translate GameHQ-specific installer messages")
    build_script = (ROOT / "packaging" / "build-setup.ps1").read_text(encoding="utf-8")
    for marker in ("generate_inno_languages.py", "test_inno_languages.py", "--require-compiler"):
        if marker not in build_script:
            fail(f"installer build does not enforce {marker}")

    toolchain_text = (ROOT / "packaging" / "inno-toolchain.psd1").read_text(encoding="utf-8")
    version = parse_psd1_value(toolchain_text, "Version")
    archive_hash = parse_psd1_value(toolchain_text, "Sha256")
    compiler_hash = parse_psd1_value(toolchain_text, "CompilerSha256")
    if version != provenance["toolchain"]["version"]:
        fail("toolchain and language provenance versions differ")
    if archive_hash != provenance["toolchain"]["archive_sha256"]:
        fail("toolchain and language provenance archive hashes differ")
    if compiler_hash != provenance["toolchain"]["compiler_sha256"]:
        fail("toolchain and language provenance compiler hashes differ")
    tag = f"is-{version.replace('.', '_')}"
    if not provenance["toolchain"]["release_url"].endswith(f"/tag/{tag}"):
        fail("toolchain release provenance is not pinned to the immutable tag")
    if not (ROOT / provenance["toolchain"]["license_file"]).is_file():
        fail("Inno Setup license evidence is missing")

    for resource in provenance["vendored_resources"]:
        path = ROOT / resource["file"]
        if sha256(path) != resource["vendored_sha256"]:
            fail(f"vendored resource hash mismatch: {resource['file']}")
        if f"/{tag}/" not in resource["upstream_url"]:
            fail(f"vendored resource URL is not immutable: {resource['file']}")
        if resource["license"] != "Inno Setup License":
            fail(f"vendored resource license is not recorded: {resource['file']}")

    compiler_root = args.compiler_root or ROOT / "tools" / "InnoSetup" / version
    if compiler_root.is_dir():
        compiler = compiler_root / "ISCC.exe"
        if not compiler.is_file() or sha256(compiler) != compiler_hash:
            fail("pinned Inno compiler executable hash mismatch")
        for relative, expected_hash in provenance["bundled_resources"].items():
            path = compiler_root / relative
            if not path.is_file() or sha256(path) != expected_hash:
                fail(f"pinned compiler resource mismatch: {relative}")
        for locale in locales.values():
            for source in locale["inno_message_file"].split(","):
                if source.startswith("compiler:") and not compiler_path(compiler_root, source).is_file():
                    fail(f"missing compiler resource for {locale['tag']}: {source}")
    elif args.require_compiler:
        fail(f"pinned Inno compiler is unavailable: {compiler_root}")

    print(f"Inno locale mapping audit passed (16 locales, pinned {version} resources)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
