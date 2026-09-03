#!/usr/bin/env python3
"""Focused audit for the sixteen-locale production launch infrastructure."""

from __future__ import annotations

import json
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
LAUNCH_LOCALES = {
    "en-US", "zh-Hans", "ru-RU", "es-ES", "pt-BR", "de-DE", "ja-JP",
    "fr-FR", "pl-PL", "ko-KR", "zh-Hant", "tr-TR", "th-TH", "es-419",
    "uk-UA", "it-IT",
}
PROMOTED = {
    "th-TH": "gamehq_th_TH",
    "es-419": "gamehq_es_419",
    "uk-UA": "gamehq_uk_UA",
    "it-IT": "gamehq_it_IT",
}
LATIN_AMERICAN_SPANISH_ALIASES = {
    "es-MX", "es-AR", "es-BO", "es-CL", "es-CO", "es-CR", "es-DO",
    "es-EC", "es-GT", "es-HN", "es-NI", "es-PA", "es-PE", "es-PR",
    "es-PY", "es-SV", "es-US", "es-UY", "es-VE",
}


def read_json(relative: str) -> dict[str, object]:
    return json.loads((ROOT / relative).read_text(encoding="utf-8"))


def catalog_messages(relative: str) -> dict[str, ET.Element]:
    root = ET.parse(ROOT / relative).getroot()
    return {message.get("id", ""): message for message in root.findall(".//message")}


class LaunchPortfolioTest(unittest.TestCase):
    def test_manifest_exposes_exactly_sixteen_production_locales(self) -> None:
        registry = read_json("i18n/locales.json")
        locales = {entry["tag"]: entry for entry in registry["locales"]}
        enabled = {
            tag for tag, entry in locales.items()
            if entry["tier"] == 1 and entry["state"] == "enabled"
        }
        self.assertEqual(LAUNCH_LOCALES, enabled)
        self.assertEqual("reserve", locales["cs-CZ"]["state"])
        self.assertFalse(any(entry["tier"] == 2 for entry in locales.values()))
        self.assertFalse({"en-XA", "ar-XB"} & set(locales))

        aliases = registry["aliases"]
        self.assertTrue(LATIN_AMERICAN_SPANISH_ALIASES <= set(aliases))
        for alias in LATIN_AMERICAN_SPANISH_ALIASES:
            self.assertEqual("es-419", aliases[alias])
        for tag, catalog in PROMOTED.items():
            self.assertEqual(catalog, locales[tag]["qt_catalog"])
            self.assertEqual("en-US" if tag != "es-419" else "es-ES", locales[tag]["fallback"])

    def test_promoted_catalogs_are_synchronized_for_safe_english_fallback(self) -> None:
        source = catalog_messages("i18n/app/gamehq_en_US.ts")
        self.assertEqual(830, len(source))
        for tag, catalog in PROMOTED.items():
            with self.subTest(locale=tag):
                target = catalog_messages(f"i18n/app/{catalog}.ts")
                self.assertEqual(set(source), set(target))
                for message_id, message in target.items():
                    self.assertEqual(
                        source[message_id].findtext("source"), message.findtext("source"), message_id
                    )
                    translation = message.find("translation")
                    self.assertIsNotNone(translation, message_id)
                    self.assertEqual("unfinished", translation.get("type"), message_id)
                    self.assertFalse("".join(translation.itertext()).strip(), message_id)

    def test_generated_state_resources_and_policy_cover_the_portfolio(self) -> None:
        status = read_json("i18n/extracted/catalog-status.json")["locales"]
        state = read_json("i18n/state/translations.json")["locales"]
        self.assertEqual(LAUNCH_LOCALES, set(status))
        for tag, catalog in PROMOTED.items():
            with self.subTest(locale=tag):
                self.assertEqual(830, len(status[tag]["missing"]))
                self.assertTrue(state[tag]["catalog_present"])
                self.assertTrue(state[tag]["enabled"])
                self.assertEqual(830, len(state[tag]["completeness"]["application"]["missing"]))
                style = read_json(f"i18n/style/{tag}.json")
                self.assertEqual(2, style["schema_version"])
                self.assertTrue(style["contextual_terminology"])

        cmake = (ROOT / "src/CMakeLists.txt").read_text(encoding="utf-8")
        for catalog in PROMOTED.values():
            self.assertIn(f"i18n/app/{catalog}.ts", cmake)
        self.assertIn("GAMEHQ_ENABLE_PSEUDO_LOCALES", cmake)

    def test_current_documents_do_not_reintroduce_delayed_rollout(self) -> None:
        architecture = (ROOT / "docs/localization/architecture.md").read_text(encoding="utf-8")
        self.assertIn("Production launch locales** (16)", architecture)
        self.assertNotIn("**Tier 2** (follow-up", architecture)
        playnite = (
            ROOT / "integrations/playnite/src/GameHQ.Playnite/Localization/README.md"
        ).read_text(encoding="utf-8")
        self.assertIn("owner-approved launch portfolio", playnite)
        self.assertNotIn("Tier 2", playnite)


if __name__ == "__main__":
    unittest.main()
