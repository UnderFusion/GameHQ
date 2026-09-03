#!/usr/bin/env python3
"""Focused tests for localization release-readiness governance."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools" / "i18n"
sys.path.insert(0, str(TOOLS))

import release_readiness as readiness  # noqa: E402


class ReleaseReadinessTest(unittest.TestCase):
    def setUp(self) -> None:
        self.evidence = readiness.build_evidence(ROOT)

    def test_committed_snapshot_is_byte_current_and_deterministic(self) -> None:
        path = ROOT / "i18n/release/readiness-0.7.7.json"
        first = readiness.json_bytes(self.evidence)
        second = readiness.json_bytes(readiness.build_evidence(ROOT))
        self.assertEqual(first, second)
        self.assertEqual(first, path.read_bytes())

    def test_portfolio_order_and_release_authority_stay_separate(self) -> None:
        portfolio = self.evidence["portfolio"]
        self.assertEqual(16, len(portfolio["production_locales"]))
        self.assertEqual(["en-XA", "ar-XB"], portfolio["development_only"])
        self.assertEqual(["cs-CZ"], portfolio["reserve"])
        self.assertEqual(list(range(1, 9)),
                         [step["order"] for step in self.evidence["workflow"]])
        self.assertEqual("p8-3 only", self.evidence["workflow"][6]["gate"])
        self.assertEqual("p8-4 only", self.evidence["workflow"][7]["gate"])
        self.assertTrue(all(item["state"] == "pending" for item in self.evidence["handoff"]))
        self.assertFalse(
            self.evidence["provenance_contract"]
            ["machine_output_self_certifies_linguistic_quality"]
        )
        self.assertIsNone(self.evidence["candidate"]["date"])
        self.assertEqual("not_requested", self.evidence["candidate"]["release_authorization"])
        self.assertEqual("prohibited", self.evidence["candidate"]["publication_state"])

    def test_every_locale_carries_sidebar_and_truthful_qa_evidence(self) -> None:
        expected_ids = list(readiness.REQUIRED_SURFACES)
        self.assertEqual(16, len(self.evidence["locales"]))
        for locale, value in self.evidence["locales"].items():
            self.assertEqual(expected_ids, [entry["id"] for entry in value["required_surfaces"]], locale)
            self.assertTrue(all(entry["catalog_translation_hash"]
                                for entry in value["required_surfaces"]), locale)
            review_path = ROOT / f"i18n/quality/reviews/{locale}.json"
            if review_path.is_file():
                self.assertEqual("contextually_reviewed", value["linguistic_qa"]["state"])
                self.assertEqual(f"i18n/quality/reviews/{locale}.json",
                                 value["linguistic_qa"]["artifact"]["path"])
            else:
                self.assertEqual(
                    {"state": "pending", "authority": "p8-3", "artifact": None},
                    value["linguistic_qa"], locale,
                )

    def test_correction_ledger_rejects_unscoped_private_or_false_acceptance(self) -> None:
        corrections = readiness.read_json(ROOT / "i18n/release/corrections.json")
        tags = list(self.evidence["portfolio"]["production_locales"])
        extracted = readiness.read_json(ROOT / "i18n/extracted/messages.json")
        locales = self.evidence["locales"]
        readiness.validate_corrections(corrections, ROOT, tags, extracted, locales)
        entry = corrections["entries"][0]
        self.assertEqual("pl-PL", entry["locale"])
        self.assertEqual("gamehq.navigation.about", entry["message_id"])
        self.assertEqual("contextually_reviewed", entry["review_state"])

        mutations = []
        unsupported = copy.deepcopy(corrections)
        unsupported["entries"][0]["locale"] = "cs-CZ"
        mutations.append((unsupported, "unsupported production locale"))
        stale = copy.deepcopy(corrections)
        stale["entries"][0]["source_hash"] = "0" * 64
        mutations.append((stale, "stale source hash"))
        private = copy.deepcopy(corrections)
        private["entries"][0]["telemetry"] = "forbidden"
        mutations.append((private, "forbidden private field"))
        false_acceptance = copy.deepcopy(corrections)
        false_acceptance["entries"][0]["review_state"] = "linguistically_accepted"
        mutations.append((false_acceptance, "only human contextual review"))
        false_context = copy.deepcopy(corrections)
        false_context["entries"][0]["method"] = "machine_verification"
        mutations.append((false_context, "explicit method"))
        for document, diagnostic in mutations:
            with self.subTest(diagnostic=diagnostic):
                with self.assertRaisesRegex(readiness.ReadinessError, diagnostic):
                    readiness.validate_corrections(document, ROOT, tags, extracted, locales)

    def test_check_mode_rejects_stale_output_without_rewriting_it(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gamehq-readiness-") as temporary:
            output = Path(temporary) / "readiness.json"
            generated = subprocess.run(
                [sys.executable, str(TOOLS / "release_readiness.py"),
                 "--root", str(ROOT), "--output", str(output)],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            self.assertEqual(0, generated.returncode, generated.stdout + generated.stderr)
            output.write_bytes(output.read_bytes() + b"\n")
            stale = output.read_bytes()
            checked = subprocess.run(
                [sys.executable, str(TOOLS / "release_readiness.py"),
                 "--root", str(ROOT), "--output", str(output), "--check"],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            self.assertNotEqual(0, checked.returncode)
            self.assertIn("stale release-readiness evidence", checked.stdout + checked.stderr)
            self.assertEqual(stale, output.read_bytes())

    def test_fast_ci_requires_readiness_evidence_and_focused_fixtures(self) -> None:
        gate = (TOOLS / "ci.ps1").read_text(encoding="utf-8")
        self.assertIn("release_readiness.py", gate)
        self.assertIn("test_release_readiness.py", gate)
        for forbidden in ("Invoke-WebRequest", "Invoke-RestMethod", "curl ", "secrets."):
            self.assertNotIn(forbidden.casefold(), gate.casefold())


if __name__ == "__main__":
    unittest.main(verbosity=2)
