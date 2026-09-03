#!/usr/bin/env python3
"""Focused regression tests for the sixteen-locale acceptance report merger."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from multilingual_acceptance_report import SUPPORT_URL, build_report


ROOT = Path(__file__).resolve().parents[2]


class MultilingualAcceptanceReportTest(unittest.TestCase):
    def fixtures(self) -> tuple[dict, dict, dict]:
        manifest = json.loads((ROOT / "i18n" / "locales.json").read_text(encoding="utf-8"))
        locales = [entry for entry in manifest["locales"] if entry["state"] == "enabled"]
        package_rows = []
        for entry in locales:
            tag = entry["tag"]
            package_rows.append({
                "locale": tag, "requested_locale": tag, "effective_locale": tag,
                "live_switch": True, "repeated_switch": True, "system_resolution": True,
                "persistence_restart": True, "installer_handoff_consumed": True,
                "whole_document_notes": True, "about": f"About {tag}",
                "support_gamehq": f"Support {tag}", "formatted_date": "2026-09-03",
                "release_count": 4, "fallback_documents": 3,
            })
        package = {
            "runtime_locale_count": 16, "update_authorization_input": False,
            "external_browser_opened": False, "locales": package_rows,
            "payload_application": {"sha256": "a" * 64},
            "portable_application": {"sha256": "a" * 64},
            "update_application": {"sha256": "a" * 64},
        }
        bootstrap = {
            "checks_passed": 27,
            "checks": ["the isolated bootstrap matrix left the real GameHQ profile unchanged"],
            "mappings": [{"app_locale": entry["tag"],
                          "installer_language": entry["inno_language"]} for entry in locales],
        }
        representatives = ("en-US", "pl-PL", "zh-Hans", "zh-Hant", "ru-RU", "th-TH",
                           "es-419", "de-DE")
        regression = {
            "checks_passed": 22,
            "checks": ["the isolated regression left the real GameHQ installation metadata unchanged"],
            "critical_languages": [{"locale": tag} for tag in representatives],
        }
        return package, bootstrap, regression

    def test_merges_all_sixteen_slots_without_claiming_linguistic_acceptance(self) -> None:
        package, bootstrap, regression = self.fixtures()
        report = build_report(ROOT, package, bootstrap, regression)
        self.assertEqual(report["passed_locale_count"], 16)
        self.assertFalse(report["linguistic_acceptance"])
        self.assertEqual(report["support_url"], SUPPORT_URL)
        self.assertTrue(report["support_url_dispatch_verified"])
        self.assertFalse(report["external_browser_opened"])
        self.assertEqual(sum(row["full_installer_lifecycle_smoke"] for row in report["locales"]), 8)

    def test_missing_production_slot_fails_with_artifact_specific_diagnostic(self) -> None:
        package, bootstrap, regression = self.fixtures()
        package["locales"].pop()
        with self.assertRaisesRegex(ValueError, "package: locale order or membership"):
            build_report(ROOT, package, bootstrap, regression)

    def test_failed_runtime_fact_names_the_locale_and_check(self) -> None:
        package, bootstrap, regression = self.fixtures()
        package["locales"][8]["repeated_switch"] = False
        with self.assertRaisesRegex(ValueError, "pl-PL: runtime check failed: repeated_switch"):
            build_report(ROOT, package, bootstrap, regression)


if __name__ == "__main__":
    unittest.main(verbosity=2)
