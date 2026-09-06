#!/usr/bin/env python3
"""Focused audit for the sixteen-locale production launch infrastructure."""

from __future__ import annotations

import json
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

import linguistic_qa


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
    return {
        message.get("id", ""): message for message in root.findall(".//message")
        if message.find("translation") is None
        or message.find("translation").get("type") not in {"vanished", "obsolete"}
    }


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

    def test_enabled_catalogs_are_complete_and_synchronized(self) -> None:
        expected = linguistic_qa.source_messages(ROOT)
        source = catalog_messages("i18n/app/gamehq_en_US.ts")
        self.assertEqual(set(expected), set(source))
        for locale in read_json("i18n/locales.json")["locales"]:
            if locale["state"] != "enabled":
                continue
            catalog = locale["qt_catalog"]
            with self.subTest(locale=locale["tag"]):
                target = catalog_messages(f"i18n/app/{catalog}.ts")
                self.assertEqual(set(source), set(target))
                # Checks every active ID, duplicate IDs, unfinished text and empty plural forms.
                self.assertEqual(set(expected), set(linguistic_qa.catalog_messages(
                    ROOT / f"i18n/app/{catalog}.ts")))
                for message_id, message in target.items():
                    self.assertEqual(
                        source[message_id].findtext("source"), message.findtext("source"), message_id
                    )
                    self.assertEqual(expected[message_id]["source"], message.findtext("source"), message_id)

    def test_generated_state_resources_and_policy_cover_the_portfolio(self) -> None:
        status = read_json("i18n/extracted/catalog-status.json")["locales"]
        state = read_json("i18n/state/translations.json")["locales"]
        expected = linguistic_qa.source_messages(ROOT)
        self.assertEqual(LAUNCH_LOCALES, set(status))
        for tag in LAUNCH_LOCALES:
            with self.subTest(locale=tag):
                self.assertEqual([], status[tag]["missing"])
                self.assertEqual([], status[tag]["stale"])
                self.assertCountEqual(expected, status[tag]["unchanged"])
                self.assertTrue(state[tag]["catalog_present"])
                self.assertTrue(state[tag]["enabled"])
                self.assertEqual(set(expected), set(state[tag]["messages"]))
                completeness = state[tag]["completeness"]["application"]
                application_count = sum(message["domain"] == "application" for message in expected.values())
                self.assertEqual(application_count, completeness["total"])
                self.assertEqual(application_count, completeness["current"])
                self.assertEqual([], completeness["missing"])
                self.assertEqual([], completeness["stale"])
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
        self.assertNotIn("Tier 2", playnite)
        # Check the actual rollout contract independently of README editorial wording.
        mapping = read_json("integrations/playnite/src/GameHQ.Playnite/Localization/locale-map.json")
        self.assertEqual("Playnite", mapping["selection_owner"])
        self.assertCountEqual(LAUNCH_LOCALES, [entry["gamehq"] for entry in mapping["locales"]])
        for entry in mapping["locales"]:
            self.assertTrue((ROOT / "integrations/playnite/src/GameHQ.Playnite/Localization"
                             / entry["resource"]).is_file())


if __name__ == "__main__":
    unittest.main()
