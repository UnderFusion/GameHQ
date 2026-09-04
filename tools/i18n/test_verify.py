#!/usr/bin/env python3
"""Focused fixtures for translation state and structural verification."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


REPOSITORY = Path(__file__).resolve().parents[2]
FIXTURE = REPOSITORY / "tests" / "fixtures" / "i18n-verify" / "project"
VERIFY = REPOSITORY / "tools" / "i18n" / "verify.py"
sys.path.insert(0, str(VERIFY.parent))
import verify as verification  # noqa: E402


class VerifyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gamehq-verify-test-")
        self.root = Path(self.temporary.name) / "project"
        self.reset_fixture()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @property
    def state_path(self) -> Path:
        return self.root / "i18n" / "state" / "translations.json"

    @property
    def polish_catalog(self) -> Path:
        return self.root / "i18n" / "app" / "gamehq_pl_PL.ts"

    def reset_fixture(self) -> None:
        if self.root.exists():
            shutil.rmtree(self.root)
        shutil.copytree(FIXTURE, self.root)

    def run_verify(
        self, *arguments: str, expected: int = 0
    ) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(VERIFY), "--root", str(self.root), *arguments]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace")
        self.assertEqual(
            expected,
            result.returncode,
            msg=f"command: {command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        return result

    def read_state(self) -> dict[str, object]:
        return json.loads(self.state_path.read_text(encoding="utf-8"))

    def edit_translation(self, message_id: str, text: str) -> None:
        tree = ET.parse(self.polish_catalog)
        translation = tree.find(f".//message[@id='{message_id}']/translation")
        self.assertIsNotNone(translation)
        translation.text = text
        tree.write(self.polish_catalog, encoding="utf-8", xml_declaration=True)

    def test_reconciliation_is_targeted_and_byte_stable(self) -> None:
        self.run_verify(expected=1)
        self.run_verify("--update-state")
        first = self.state_path.read_bytes()
        state = self.read_state()
        polish = state["locales"]["pl-PL"]["messages"]

        self.assertEqual("stale", polish["gamehq.fixture.changed"]["status"])
        self.assertEqual(
            "fc41312d0ca33490a909bc15f499c895a87c6be7108c4297a96714f94b99917e",
            polish["gamehq.fixture.changed"]["source_hash"],
        )
        self.assertEqual("human_reviewed", polish["gamehq.fixture.unchanged"]["status"])
        self.assertEqual("machine_verified", polish["gamehq.fixture.files"]["status"])
        self.assertEqual("human_reviewed", polish["gamehq.fixture.markup"]["status"])
        self.assertEqual("missing", polish["gamehq.fixture.new"]["status"])
        self.assertNotIn("gamehq.fixture.removed", polish)
        self.assertEqual(
            {"total": 5, "current": 3,
             "missing": ["gamehq.fixture.new"],
             "stale": ["gamehq.fixture.changed"]},
            state["locales"]["pl-PL"]["completeness"]["application"],
        )
        self.assertTrue(all(
            entry["status"] == "human_reviewed"
            for entry in state["locales"]["en-US"]["messages"].values()
        ))

        self.run_verify("--update-state")
        self.assertEqual(first, self.state_path.read_bytes())
        self.run_verify()

    def test_translation_payload_drift_downgrades_only_that_current_entry(self) -> None:
        self.run_verify("--update-state")
        self.edit_translation("gamehq.fixture.unchanged", "Uruchom %1 w GameHQ")
        self.run_verify("--update-state")
        polish = self.read_state()["locales"]["pl-PL"]["messages"]
        self.assertEqual("stale", polish["gamehq.fixture.unchanged"]["status"])
        self.assertEqual("machine_verified", polish["gamehq.fixture.files"]["status"])
        self.assertEqual("human_reviewed", polish["gamehq.fixture.markup"]["status"])

    def test_release_mode_blocks_incomplete_enabled_locale(self) -> None:
        self.run_verify("--update-state")
        result = self.run_verify("--release", expected=1)
        self.assertIn("enabled locale pl-PL/application is incomplete (3/5)", result.stderr)

    def test_agent_contextual_review_state_is_preserved_and_validated(self) -> None:
        self.run_verify("--update-state")
        state = self.read_state()
        entry = state["locales"]["pl-PL"]["messages"]["gamehq.fixture.unchanged"]
        entry["status"] = "contextually_reviewed"
        entry["provenance"] = {"kind": "agent", "actor": "fixture-context-review"}
        entry["updated_at"] = "2026-09-03T22:45:00Z"
        self.state_path.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
        self.run_verify("--update-state")
        self.assertEqual(
            "contextually_reviewed",
            self.read_state()["locales"]["pl-PL"]["messages"]
            ["gamehq.fixture.unchanged"]["status"],
        )

        entry["provenance"] = {"kind": "machine", "actor": "fixture"}
        self.state_path.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
        result = self.run_verify(expected=2)
        self.assertIn("contextual state lacks agent provenance", result.stderr)

    def test_contextual_product_name_may_be_added_but_other_protected_tokens_may_not(self) -> None:
        self.run_verify("--update-state")
        self.edit_translation("gamehq.fixture.changed", "GameHQ — Stare zrodlo")
        self.run_verify("--update-state")

        self.edit_translation("gamehq.fixture.changed", "Stare zrodlo https://example.invalid")
        result = self.run_verify("--update-state", expected=2)
        self.assertIn("protected token differs from English source", result.stderr)

    def test_korean_particle_may_attach_to_protected_product_name(self) -> None:
        message = {
            "source": "GameHQ starts.", "placeholders": [],
            "markup_signature": [], "protected_tokens": ["GameHQ"],
            "locations": [{"file": "src/ui/qml/Settings.qml", "line": 1}],
        }
        verification.validate_translation(
            "ko-KR/gamehq.fixture.korean_particle", message, "GameHQ가 시작됩니다."
        )
        verification.validate_translation(
            "ko-KR/gamehq.fixture.korean_particle", message, "GameHQ에서만 시작됩니다."
        )
        with self.assertRaisesRegex(
            verification.VerificationError, "Korean particle is separated"
        ):
            verification.validate_translation(
                "ko-KR/gamehq.fixture.korean_particle", message,
                "설치된 GameHQ 에서만 시작됩니다."
            )

    def test_qml_conjunction_ampersand_is_not_treated_as_a_mnemonic(self) -> None:
        message = {
            "source": "Security & privacy", "placeholders": [],
            "markup_signature": [], "protected_tokens": [],
            "locations": [{"file": "src/ui/qml/Settings.qml", "line": 1}],
        }
        verification.validate_translation(
            "pl-PL/gamehq.fixture.qml", message, "Bezpieczeństwo i prywatność"
        )
        message["locations"] = [{"file": "src/ui/Menu.cpp", "line": 1}]
        with self.assertRaisesRegex(verification.VerificationError, "accelerator marker"):
            verification.validate_translation(
                "pl-PL/gamehq.fixture.menu", message, "Bezpieczeństwo i prywatność"
            )

    def test_structural_violations_fail_without_mutating_state(self) -> None:
        def missing_placeholder() -> None:
            self.edit_translation("gamehq.fixture.unchanged", "Otworz w GameHQ")

        def broken_markup() -> None:
            self.edit_translation("gamehq.fixture.markup", "<b>Otworz GameHQ")

        def wrong_plural_count() -> None:
            tree = ET.parse(self.polish_catalog)
            translation = tree.find(".//message[@id='gamehq.fixture.files']/translation")
            forms = translation.findall("numerusform")
            translation.remove(forms[-1])
            tree.write(self.polish_catalog, encoding="utf-8", xml_declaration=True)

        def changed_protected_token() -> None:
            self.edit_translation("gamehq.fixture.markup", "<b>Otworz</b> GameHQueue")

        def invalid_utf8() -> None:
            data = self.polish_catalog.read_bytes()
            self.polish_catalog.write_bytes(data.replace(b"Stare zrodlo", b"Stare \xffzrodlo"))

        def invalid_id() -> None:
            path = self.root / "i18n" / "extracted" / "messages.json"
            manifest = json.loads(path.read_text(encoding="utf-8"))
            manifest["messages"][0]["id"] = "GameHQ.invalid"
            path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

        def mojibake() -> None:
            self.edit_translation("gamehq.fixture.changed", "Stare Ãƒzrodlo")

        def non_nfc() -> None:
            self.edit_translation("gamehq.fixture.changed", "Otwo\u0301rz zrodlo")

        cases = (
            ("placeholder multiset", missing_placeholder),
            ("markup is unbalanced", broken_markup),
            ("expected 3 plural forms", wrong_plural_count),
            ("protected token differs", changed_protected_token),
            ("cannot read UTF-8 catalog", invalid_utf8),
            ("invalid ID", invalid_id),
            ("mojibake signature", mojibake),
            ("not NFC-normalized", non_nfc),
        )
        for expected_error, mutate in cases:
            with self.subTest(expected_error=expected_error):
                self.reset_fixture()
                before = self.state_path.read_bytes()
                mutate()
                result = self.run_verify("--update-state", expected=2)
                self.assertIn(expected_error, result.stderr)
                self.assertEqual(before, self.state_path.read_bytes())


def main() -> None:
    parser = argparse.ArgumentParser()
    _, unittest_arguments = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *unittest_arguments])


if __name__ == "__main__":
    main()
