#!/usr/bin/env python3
"""Acceptance tests for the offline localization CI gate."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "unsigned-beta.yml"
GATE = ROOT / "tools" / "i18n" / "ci.ps1"
GENERATOR = ROOT / "tools" / "i18n" / "generate_release_notes.py"


class LocalizationCiTest(unittest.TestCase):
    def test_pr_and_release_builds_require_the_fast_offline_gate(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("pull_request:", workflow)
        self.assertIn("branches:\n      - dev\n      - main", workflow)
        fast_start = workflow.index("  localization-fast:")
        build_start = workflow.index("  build-and-verify:")
        fast_job = workflow[fast_start:build_start]
        build_job = workflow[build_start:]
        self.assertIn("tools/i18n/ci.ps1", fast_job)
        self.assertIn("timeout-minutes: 10", fast_job)
        self.assertIn("needs: localization-fast", build_job)
        self.assertIn("tools/i18n/sync.ps1 -Check", build_job)
        for forbidden in ("secrets.", "pip install", "Invoke-WebRequest", "curl "):
            self.assertNotIn(forbidden, fast_job)

    def test_gate_is_local_non_mutating_and_reuses_existing_validators(self) -> None:
        gate = GATE.read_text(encoding="utf-8")
        required = (
            "locale.ps1'), 'verify'",
            "test-release-note-assets.ps1",
            "test_verify.py",
            "test_inno_languages.py",
            "test_inno_custom_messages.py",
            "test_inno_bootstrap.py",
            "test_auxiliary_surfaces.py",
            "--porcelain=v1",
        )
        for value in required:
            self.assertIn(value, gate)
        for forbidden in (
            "UpdateState", "--update-state", "requests", "urllib", "urlopen",
            "Invoke-WebRequest", "Invoke-RestMethod", "llm", "translation api",
        ):
            self.assertNotIn(forbidden.casefold(), gate.casefold())

    def test_stale_generated_locale_fails_clearly_without_rewriting_it(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gamehq-ci-stale-") as temporary:
            source = Path(temporary) / "release-notes"
            shutil.copytree(ROOT / "assets" / "release-notes", source)
            generated = source / "generated"
            stale = generated / "release-notes.pl-PL.json"
            stale.write_bytes(stale.read_bytes() + b"\n")
            before = stale.read_bytes()
            result = subprocess.run(
                [sys.executable, str(GENERATOR), "--source-root", str(source),
                 "--output-root", str(generated), "--check"],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            self.assertNotEqual(0, result.returncode)
            diagnostic = result.stdout + result.stderr
            self.assertIn("release-notes.pl-PL.json", diagnostic)
            self.assertIn("stale", diagnostic)
            self.assertEqual(before, stale.read_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)
