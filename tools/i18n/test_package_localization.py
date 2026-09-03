#!/usr/bin/env python3
"""Contract tests for localization-aware production package validation."""

from __future__ import annotations

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class PackageLocalizationTest(unittest.TestCase):
    def test_packaged_probe_uses_exact_production_portfolio(self) -> None:
        manifest = json.loads((ROOT / "i18n/locales.json").read_text(encoding="utf-8"))
        expected = [entry["tag"] for entry in manifest["locales"]
                    if entry["state"] == "enabled"]
        self.assertEqual(16, len(expected))
        probe = (ROOT / "src/app/PackagedLocalizationProbe.cpp").read_text(encoding="utf-8")
        positions = [probe.index(f'QStringLiteral("{locale}")') for locale in expected]
        self.assertEqual(sorted(positions), positions)
        for forbidden in ("en-XA", "ar-XB", "cs-CZ"):
            self.assertIn(f'QStringLiteral("{forbidden}")', probe)
        for representative in ("en-US", "pl-PL", "zh-Hant", "th-TH"):
            self.assertGreaterEqual(probe.count(f'QStringLiteral("{representative}")'), 2)

    def test_actual_payload_and_archive_bytes_are_validated(self) -> None:
        validator = (ROOT / "packaging/test-localized-package.ps1").read_text(encoding="utf-8")
        for required in (
            "--localization-assets-self-test",
            "production_locale_count -eq 16",
            "catalog_count -eq 16",
            "release_note_bundle_count -eq 16",
            "Get-ZipEntryEvidence $portable 'app/GameHQ.exe'",
            "Get-ZipEntryEvidence $update 'app/GameHQ.exe'",
            "does not match the localization-verified payload application bytes",
            "contains loose localization data",
            "update_authorization_input = $false",
        ):
            self.assertIn(required, validator)

    def test_release_and_installer_paths_require_artifact_probe(self) -> None:
        release = (ROOT / "packaging/validate-release.ps1").read_text(encoding="utf-8")
        installer = (ROOT / "packaging/test-installer-regression.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn("test-localized-package.ps1", release)
        self.assertIn("test-installer-language-acceptance.ps1", release)
        self.assertIn("localization = [ordered]@{", release)
        self.assertIn("updateAuthorizationInput = $false", release)
        self.assertIn("--localization-assets-self-test", installer)
        self.assertIn("production Setup installed the same verified sixteen-locale", installer)


if __name__ == "__main__":
    unittest.main(verbosity=2)
