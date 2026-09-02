#!/usr/bin/env python3
"""Focused deterministic tests for the GameHQ localization synchronizer."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


REPOSITORY = Path(__file__).resolve().parents[2]
FIXTURE = REPOSITORY / "tests" / "fixtures" / "i18n-sync" / "project"
SYNC = REPOSITORY / "tools" / "i18n" / "sync.py"
LUPDATE = ""


def snapshot(directory: Path) -> dict[str, bytes]:
    return {
        path.relative_to(directory).as_posix(): path.read_bytes()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


class SyncTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gamehq-sync-test-")
        self.root = Path(self.temporary.name) / "project"
        shutil.copytree(FIXTURE, self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_sync(self, *arguments: str, expected: int = 0) -> subprocess.CompletedProcess[str]:
        command = [
            sys.executable,
            str(SYNC),
            "--root",
            str(self.root),
            "--lupdate",
            LUPDATE,
            *arguments,
        ]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace")
        self.assertEqual(
            expected,
            result.returncode,
            msg=f"command: {command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        return result

    def test_sync_is_byte_stable_and_reports_catalog_state(self) -> None:
        first = self.run_sync()
        self.assertIn("added=1", first.stdout)
        self.assertIn("obsolete=1", first.stdout)
        first_snapshot = snapshot(self.root / "i18n")

        second = self.run_sync()
        self.assertIn("updated 0 file(s)", second.stdout)
        self.assertEqual(first_snapshot, snapshot(self.root / "i18n"))
        self.run_sync("--check")

        manifest = json.loads(
            (self.root / "i18n" / "extracted" / "messages.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            ["gamehq.fixture.files", "gamehq.fixture.new", "gamehq.fixture.open"],
            [message["id"] for message in manifest["messages"]],
        )
        files = manifest["messages"][0]
        self.assertTrue(files["plural"])
        self.assertEqual(["%n"], files["placeholders"])
        self.assertEqual(["<b>", "</b>"], files["markup_signature"])
        self.assertEqual(
            hashlib.sha256(files["source"].encode("utf-8")).hexdigest(),
            files["source_hash"],
        )

        report = json.loads(
            (self.root / "i18n" / "extracted" / "catalog-status.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(
            {
                "missing": ["gamehq.fixture.new"],
                "stale": ["gamehq.fixture.files"],
                "obsolete": ["gamehq.fixture.removed"],
                "unchanged": ["gamehq.fixture.open"],
            },
            {key: report["locales"]["pl-PL"][key] for key in (
                "missing", "stale", "obsolete", "unchanged"
            )},
        )
        polish_root = ET.parse(self.root / "i18n" / "app" / "gamehq_pl_PL.ts").getroot()
        removed = polish_root.find(".//message[@id='gamehq.fixture.removed']/translation")
        self.assertIsNotNone(removed)
        self.assertEqual("vanished", removed.get("type"))
        self.assertIn("Usunieta wiadomosc", "".join(removed.itertext()))

    def test_check_detects_divergence_without_writing_outputs(self) -> None:
        self.run_sync()
        before = snapshot(self.root / "i18n")
        source = self.root / "src" / "sample.cpp"
        source.write_text(
            source.read_text(encoding="utf-8").replace("New message", "Changed message"),
            encoding="utf-8",
        )
        result = self.run_sync("--check", expected=1)
        self.assertIn("i18n synchronization required", result.stderr)
        self.assertEqual(before, snapshot(self.root / "i18n"))

    def test_invalid_source_contracts_fail_cleanly(self) -> None:
        source = self.root / "src" / "sample.cpp"
        original = source.read_text(encoding="utf-8")
        cases = {
            "missing source comment": original.replace('//% "New message"\n', ""),
            "invalid message ID": original.replace(
                "gamehq.fixture.new", "GameHQ.fixture.new"
            ),
            "conflicting English source": original + (
                '\n//% "Conflicting message"\n'
                'QString conflict() { return qtTrId("gamehq.fixture.open"); }\n'
            ),
        }
        for expected_error, content in cases.items():
            with self.subTest(expected_error=expected_error):
                source.write_text(content, encoding="utf-8")
                result = self.run_sync(expected=2)
                self.assertIn(expected_error, result.stderr)
                source.write_text(original, encoding="utf-8")


def main() -> None:
    global LUPDATE
    parser = argparse.ArgumentParser()
    parser.add_argument("--lupdate", required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    LUPDATE = arguments.lupdate
    unittest.main(argv=[sys.argv[0], *unittest_arguments])


if __name__ == "__main__":
    main()
