#!/usr/bin/env python3

from __future__ import annotations

import json
import re
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import generate_pseudo


ROOT = Path(__file__).resolve().parents[2]
PLACEHOLDERS = re.compile(r"%(?:L?[1-9][0-9]*|n)")
MARKUP = re.compile(r"</?[A-Za-z][^>]*>")
URLS = re.compile(r"https?://[^\s<>]+")


def messages(path: Path) -> dict[str, ET.Element]:
    return {message.get("id", ""): message for message in ET.parse(path).findall("./context/message")}


def translated_forms(message: ET.Element) -> list[str]:
    translation = message.find("translation")
    forms = translation.findall("numerusform") if translation is not None else []
    return [(form.text or "") for form in forms] if forms else [(translation.text or "")]


class PseudoLocaleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.first = tempfile.TemporaryDirectory()
        self.second = tempfile.TemporaryDirectory()
        self.addCleanup(self.first.cleanup)
        self.addCleanup(self.second.cleanup)
        self._generate(Path(self.first.name))
        self._generate(Path(self.second.name))

    def _generate(self, output: Path) -> None:
        tokens = generate_pseudo._manifest_tokens(ROOT / "i18n/extracted/messages.json")
        source = ROOT / "i18n/app/gamehq_en_US.ts"
        generate_pseudo.generate_catalog(source, output / "gamehq_en_XA.ts", "en-XA", tokens)
        generate_pseudo.generate_catalog(source, output / "gamehq_ar_XB.ts", "ar-XB", tokens)
        generate_pseudo.generate_development_manifest(
            ROOT / "i18n/locales.json", output / "locales.json")

    def test_generation_is_deterministic_complete_and_protocol_safe(self) -> None:
        source = messages(ROOT / "i18n/app/gamehq_en_US.ts")
        metadata = json.loads((ROOT / "i18n/extracted/messages.json").read_text(encoding="utf-8"))
        protected = {entry["id"]: entry["protected_tokens"] for entry in metadata["messages"]}
        for filename, locale, form_count in (
            ("gamehq_en_XA.ts", "en_XA", 2),
            ("gamehq_ar_XB.ts", "ar_XB", 6),
        ):
            first = Path(self.first.name) / filename
            second = Path(self.second.name) / filename
            self.assertEqual(first.read_bytes(), second.read_bytes())
            tree = ET.parse(first)
            self.assertEqual(tree.getroot().get("language"), locale)
            target = messages(first)
            self.assertEqual(set(target), set(source))
            for message_id, source_message in source.items():
                source_text = source_message.findtext("source", default="")
                forms = translated_forms(target[message_id])
                if source_message.get("numerus") == "yes":
                    self.assertEqual(len(forms), form_count)
                for value in forms:
                    self.assertNotIn("gamehq.", value)
                    self.assertEqual(PLACEHOLDERS.findall(value), PLACEHOLDERS.findall(source_text))
                    self.assertEqual(MARKUP.findall(value), MARKUP.findall(source_text))
                    self.assertEqual(URLS.findall(value), URLS.findall(source_text))
                    for token in protected.get(message_id, []):
                        self.assertEqual(value.count(token), source_text.count(token))
            all_source = "".join(message.findtext("source", default="") for message in source.values())
            all_target = "".join(form[0] for message in target.values()
                                 if (form := translated_forms(message)))
            if locale == "en_XA":
                expansion = len(all_target) / len(all_source)
                self.assertGreaterEqual(expansion, 1.35)
                self.assertLessEqual(expansion, 1.40)
                self.assertTrue(all(value.startswith("⟦") and value.endswith("⟧")
                                    for message in target.values() for value in translated_forms(message)))
            else:
                self.assertTrue(all(value.startswith("⟫\u2067") and value.endswith("\u2069⟪")
                                    for message in target.values() for value in translated_forms(message)))

    def test_only_generated_development_manifest_contains_pseudo_locales(self) -> None:
        production = json.loads((ROOT / "i18n/locales.json").read_text(encoding="utf-8"))
        self.assertFalse({"en-XA", "ar-XB"} & {entry["tag"] for entry in production["locales"]})
        development = json.loads((Path(self.first.name) / "locales.json").read_text(encoding="utf-8"))
        entries = {entry["tag"]: entry for entry in development["locales"]}
        for tag in ("en-XA", "ar-XB"):
            self.assertEqual(entries[tag]["state"], "internal")
            self.assertEqual(entries[tag]["completeness_policy"], "never_embed")
        cmake = (ROOT / "src/CMakeLists.txt").read_text(encoding="utf-8")
        conditional = cmake.index("if(GAMEHQ_ENABLE_PSEUDO_LOCALES)")
        pseudo_catalog = cmake.index("gamehq_en_XA.ts")
        conditional_end = cmake.index("endif()", conditional)
        self.assertLess(conditional, pseudo_catalog)
        self.assertLess(pseudo_catalog, conditional_end)
        root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(root_cmake, r"GAMEHQ_ENABLE_PSEUDO_LOCALES\s*\n[^\n]*\n\s*OFF\)")
        workflow = (ROOT / ".github/workflows/unsigned-beta.yml").read_text(encoding="utf-8")
        source_validation = (ROOT / "packaging/validate-source.ps1").read_text(encoding="utf-8")
        developer_launcher = (ROOT / "start.bat").read_text(encoding="utf-8")
        self.assertIn("-DGAMEHQ_ENABLE_PSEUDO_LOCALES=OFF", workflow)
        self.assertIn("-DGAMEHQ_ENABLE_PSEUDO_LOCALES=OFF", source_validation)
        self.assertIn("-DGAMEHQ_ENABLE_PSEUDO_LOCALES=ON", developer_launcher)

    def test_placeholders_markup_urls_shortcuts_and_physical_glyphs_are_invariant(self) -> None:
        source = "%1 GameHQ Ctrl+Shift+E https://example.invalid/x HDR L1/R1 ← → <b>Ready</b>"
        protected = ["GameHQ"]
        for locale in ("en-XA", "ar-XB"):
            value = generate_pseudo.pseudo_text(source, locale, protected)
            for token in ("%1", "GameHQ", "Ctrl+Shift+E", "https://example.invalid/x",
                          "HDR", "L1/R1", "←", "→", "<b>", "</b>"):
                self.assertIn(token, value)


if __name__ == "__main__":
    unittest.main()
