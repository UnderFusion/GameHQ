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
        self.assertIn("branches:\n      - dev\n      - main\n      - 'release/**'", workflow)
        fast_start = workflow.index("  localization-fast:")
        final_start = workflow.index("  localization-final:")
        build_start = workflow.index("  build-and-verify:")
        fast_job = workflow[fast_start:final_start]
        final_job = workflow[final_start:build_start]
        build_job = workflow[build_start:]
        self.assertIn("tools/i18n/ci.ps1", fast_job)
        self.assertIn("timeout-minutes: 10", fast_job)
        self.assertIn("tools/i18n/ci.ps1 -Mode final", final_job)
        self.assertIn("timeout-minutes: 10", final_job)
        self.assertIn("needs: [localization-fast, localization-final]", build_job)
        self.assertIn("tools/i18n/sync.ps1 -Check", build_job)
        for forbidden in ("secrets.", "pip install", "Invoke-WebRequest", "curl "):
            self.assertNotIn(forbidden, fast_job)
            self.assertNotIn(forbidden, final_job)

    def test_lifecycle_gates_are_routed_by_branch_and_never_by_the_manifest(self) -> None:
        """A development ref must keep failing on a finalized tree, and a release
        ref must be validated as one. Neither gate may infer which it is."""
        workflow = WORKFLOW.read_text(encoding="utf-8")
        fast_start = workflow.index("  localization-fast:")
        final_start = workflow.index("  localization-final:")
        fast_job = workflow[fast_start:final_start]
        final_job = workflow[final_start:workflow.index("  build-and-verify:")]
        self.assertIn("github.ref != 'refs/heads/main'", fast_job)
        self.assertIn("!startsWith(github.ref, 'refs/heads/release/')", fast_job)
        self.assertIn("github.ref == 'refs/heads/main'", final_job)
        self.assertIn("startsWith(github.ref, 'refs/heads/release/')", final_job)
        self.assertNotIn("-Mode final", fast_job)

        gate = GATE.read_text(encoding="utf-8")
        # Every production-neutral check runs in both modes; only the validator's
        # own candidate-evidence fixtures are development-ref work.
        neutral_start = gate.index("$pythonChecks = @(")
        neutral = gate[neutral_start:gate.index("foreach ($check in $pythonChecks)")]
        for script in ("test_verify.py", "test_inno_languages.py", "test_inno_custom_messages.py",
                       "test_inno_bootstrap.py", "test_auxiliary_surfaces.py", "test_ci.py",
                       "test_linguistic_qa.py"):
            self.assertIn(script, neutral)
        self.assertNotIn("test_release_readiness.py", neutral)
        self.assertIn("if ($Mode -eq 'candidate') {", gate)
        self.assertIn("[ValidateSet('candidate', 'final')]", gate)
        self.assertIn("$Mode = 'candidate'", gate)
        # The candidate command keeps its exact meaning; final mode is additive.
        self.assertIn("'release_readiness.py'), '--check'", gate)
        self.assertIn("'--mode', 'final'", gate)
        # Prose may describe the lifecycle; the gate may not read it.
        for forbidden in ("localization_launch", "manifest.json", "linguistic-state"):
            self.assertNotIn(forbidden, gate)

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
