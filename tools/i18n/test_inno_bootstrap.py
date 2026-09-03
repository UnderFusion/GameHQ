#!/usr/bin/env python3
"""Validate the manifest-driven one-shot installer language handoff."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def fail(message: str) -> None:
    raise AssertionError(message)


def main() -> int:
    manifest = json.loads((ROOT / "i18n" / "locales.json").read_text(encoding="utf-8"))
    locales = [
        locale
        for locale in manifest["locales"]
        if locale.get("state") == "enabled" and locale.get("tier") == 1
    ]
    expected = {
        locale["inno_language"]: locale["inno_app_locale"] for locale in locales
    }
    if len(expected) != 16 or set(expected.values()) != {locale["tag"] for locale in locales}:
        fail("bootstrap must map exactly sixteen installer languages to semantic locales")

    generated = (
        ROOT / "packaging" / "generated" / "InnoLanguageBootstrap.iss"
    ).read_text(encoding="utf-8")
    actual = dict(
        re.findall(
            r"CompareText\(Language, '([^']+)'\) = 0 then\s+Result := '([^']+)'",
            generated,
        )
    )
    if actual != expected:
        fail("generated bootstrap mapping differs from the locale manifest")
    if "Result := '';" not in generated:
        fail("unknown installer languages do not fail safely without a bootstrap")
    if any(tag in generated for tag in ("en-XA", "ar-XB")):
        fail("development pseudo-locales leaked into the installer handoff")
    for tag in ("zh-Hans", "zh-Hant", "pt-BR", "es-ES", "es-419"):
        if list(actual.values()).count(tag) != 1:
            fail(f"critical semantic locale is not distinct: {tag}")

    script = (ROOT / "packaging" / "GameHQ.iss").read_text(encoding="utf-8")
    required = (
        '#include "generated\\InnoLanguageBootstrap.iss"',
        "CanonicalLocaleForInstallerLanguage(ActiveLanguage)",
        "BootstrapLanguageOffered",
        "BootstrapExistingInstall",
        "BootstrapExistingProfile",
        "BootstrapHadValue",
        "CurStep = ssPostInstall",
        "FileExists(ExpandConstant('{app}\\portable.flag'))",
    )
    for marker in required:
        if marker not in script:
            fail(f"installer bootstrap lifecycle is missing {marker}")
    if "SaveStringToFile" in script or "ui.language" in script:
        fail("Setup must not edit the application's config or preference directly")
    if re.search(r"ActiveLanguage\s*=", script):
        fail("Setup must not contain handwritten locale branches")

    consumer = (ROOT / "src" / "localization" / "LanguagePreference.cpp").read_text(
        encoding="utf-8"
    )
    for marker in (
        'kBootstrapValue = "BootstrapLanguage"',
        "m_registry->canonicalTag(trimmed)",
        "bootstrap.removeValue()",
        "m_config->hasExplicitValue",
    ):
        if marker not in consumer:
            fail(f"application consumer contract is missing {marker}")

    tests = (ROOT / "tests" / "tst_languagepreference.cpp").read_text(encoding="utf-8")
    for test_name in (
        "registryProducerConsumerRoundTripIsOneShot",
        "existingExplicitPreferenceWins",
        "explicitSystemSurvivesReloadAndBlocksBootstrap",
        "invalidBootstrapFailsSafely",
        "consumedBootstrapIsNotReplayed",
        "upgradeBootstrapNeverOverwritesExistingChoice",
        "portableModeIgnoresInstallerBootstrap",
    ):
        if test_name not in tests:
            fail(f"application consumer coverage is missing {test_name}")

    print("Inno one-shot language bootstrap audit passed (16 semantic mappings)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"ERROR: {error}")
        raise SystemExit(1)
