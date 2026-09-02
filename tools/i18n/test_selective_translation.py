#!/usr/bin/env python3
"""End-to-end selective translation fixture with no live provider."""

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
FIXTURE = REPOSITORY / "tests" / "fixtures" / "i18n-selective"
SYNC = REPOSITORY / "tools" / "i18n" / "sync.py"
VERIFY = REPOSITORY / "tools" / "i18n" / "verify.py"
CHECK_DIFF = REPOSITORY / "tools" / "i18n" / "check_diff.py"
APPLY = REPOSITORY / "tools" / "i18n" / "apply_response.py"
UPDATED_AT = "2026-09-02T12:00:00Z"
LUPDATE = ""
sys.path.insert(0, str(REPOSITORY / "tools" / "i18n"))
import sync as extraction  # noqa: E402
import verify  # noqa: E402


def snapshot(directory: Path) -> dict[str, bytes]:
    return {
        path.relative_to(directory).as_posix(): path.read_bytes()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def translation_nodes(path: Path) -> dict[str, str]:
    root = ET.parse(path).getroot()
    return {
        str(message.get("id")): ET.tostring(
            message.find("translation"), encoding="unicode"
        )
        for message in root.findall("./context/message")
        if message.get("id")
    }


class SelectiveTranslationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gamehq-selective-test-")
        self.root = Path(self.temporary.name) / "project"
        shutil.copytree(FIXTURE / "project", self.root)
        self.work = self.root / "out"
        self.work.mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_tool(
        self,
        script: Path,
        *arguments: str,
        expected: int = 0,
    ) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(script), "--root", str(self.root), *arguments]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace")
        self.assertEqual(
            expected,
            result.returncode,
            msg=f"command: {command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        return result

    def run_sync(self) -> subprocess.CompletedProcess[str]:
        return self.run_tool(SYNC, "--lupdate", LUPDATE)

    def run_verify(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return self.run_tool(VERIFY, *arguments)

    def run_check(
        self, base: Path, output: Path, expected: int
    ) -> subprocess.CompletedProcess[str]:
        return self.run_tool(
            CHECK_DIFF,
            "--base-manifest", str(base),
            "--policy-root", str(REPOSITORY),
            "--output", str(output),
            expected=expected,
        )

    def run_apply(
        self, workset: Path, responses: Path, expected: int
    ) -> subprocess.CompletedProcess[str]:
        return self.run_tool(
            APPLY,
            "--policy-root", str(REPOSITORY),
            "--workset", str(workset),
            "--response-dir", str(responses),
            "--updated-at", UPDATED_AT,
            expected=expected,
        )

    @property
    def state_path(self) -> Path:
        return self.root / "i18n" / "state" / "translations.json"

    def seed_reviewed_state(self) -> None:
        manifest_path = self.root / "i18n" / "extracted" / "messages.json"
        registry_path = self.root / "i18n" / "locales.json"
        _, messages = verify.load_manifest(manifest_path)
        source_locale, locales = verify.load_registry(registry_path)
        state = verify.reconcile(
            self.root, manifest_path, registry_path, self.state_path
        )
        for locale in locales:
            tag = str(locale["tag"])
            if tag == source_locale:
                continue
            catalog = self.root / "i18n" / "app" / f"{locale['qt_catalog']}.ts"
            payloads = verify.read_catalog(catalog, tag, messages)
            entries: dict[str, dict[str, object]] = {}
            for message_id, message in messages.items():
                payload = payloads[message_id]
                self.assertIsNotNone(payload)
                entries[message_id] = {
                    "domain": message["domain"],
                    "source_hash": message["source_hash"],
                    "translation_hash": verify.translation_hash(payload["forms"]),
                    "status": "human_reviewed",
                    "provenance": {"kind": "human", "actor": "fixture-reviewer"},
                    "updated_at": "2026-09-01T12:00:00Z",
                }
            state["locales"][tag]["messages"] = {
                key: entries[key] for key in sorted(entries)
            }
            state["locales"][tag]["completeness"] = verify.completeness_for(
                messages, entries
            )
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.state_path.write_bytes(verify.json_bytes(state))
        self.run_verify()

    def invalid_responses(self, name: str, mutation) -> Path:
        destination = self.work / name
        shutil.copytree(FIXTURE / "expected-responses", destination)
        polish = destination / "pl-PL.json"
        response = json.loads(polish.read_text(encoding="utf-8"))
        mutation(response)
        polish.write_text(
            json.dumps(response, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        return destination

    def test_selective_fixture_is_atomic_exact_and_byte_stable(self) -> None:
        self.run_sync()
        self.seed_reviewed_state()
        base_manifest = self.work / "base-messages.json"
        shutil.copy2(
            self.root / "i18n" / "extracted" / "messages.json", base_manifest
        )

        shutil.copy2(FIXTURE / "changed" / "sample.cpp", self.root / "src" / "sample.cpp")
        self.run_sync()
        self.run_verify("--update-state")
        pending_workset = self.work / "pending-workset.json"
        result = self.run_check(base_manifest, pending_workset, expected=1)
        self.assertIn("pending=4", result.stdout)
        workset = json.loads(pending_workset.read_text(encoding="utf-8"))
        self.assertEqual(
            {
                "added": ["gamehq.fixture.captures"],
                "changed": ["gamehq.fixture.launch"],
                "removed": ["gamehq.fixture.removed"],
                "unchanged": ["gamehq.fixture.keep"],
            },
            workset["delta"],
        )
        for tag in ("pl-PL", "zh-Hans"):
            self.assertEqual(
                ["gamehq.fixture.captures", "gamehq.fixture.launch"],
                workset["locales"][tag]["required"],
            )

        before_apply = snapshot(self.root / "i18n")
        state_before = json.loads(self.state_path.read_text(encoding="utf-8"))
        self.assertEqual(
            "human_reviewed",
            state_before["locales"]["pl-PL"]["messages"]["gamehq.fixture.keep"]["status"],
        )
        self.assertEqual(
            "stale",
            state_before["locales"]["pl-PL"]["messages"]["gamehq.fixture.launch"]["status"],
        )
        self.assertEqual(
            "missing",
            state_before["locales"]["pl-PL"]["messages"]["gamehq.fixture.captures"]["status"],
        )

        invalid_cases = {
            "stale": (
                lambda response: response["units"][0].update({"source_hash": "0" * 64}),
                "stale source hash",
            ),
            "structural": (
                lambda response: response["units"][0].update(
                    {"translation": ["<b>brak struktury</b>"] * 3}
                ),
                "placeholder multiset",
            ),
        }
        for name, (mutation, error) in invalid_cases.items():
            with self.subTest(invalid=name):
                responses = self.invalid_responses(name, mutation)
                rejected = self.run_apply(pending_workset, responses, expected=2)
                self.assertIn(error, rejected.stderr)
                self.assertEqual(before_apply, snapshot(self.root / "i18n"))

        source_catalog = self.root / "i18n" / "app" / "gamehq_en_US.ts"
        catalog_status = self.root / "i18n" / "extracted" / "catalog-status.json"
        source_before = source_catalog.read_bytes()
        status_before = catalog_status.read_bytes()
        target_before = {
            tag: translation_nodes(
                self.root / "i18n" / "app" / f"gamehq_{catalog}.ts"
            )
            for tag, catalog in (("pl-PL", "pl_PL"), ("zh-Hans", "zh_Hans"))
        }

        self.run_apply(pending_workset, FIXTURE / "expected-responses", expected=0)
        self.assertEqual(source_before, source_catalog.read_bytes())
        self.assertEqual(status_before, catalog_status.read_bytes())
        state_after = json.loads(self.state_path.read_text(encoding="utf-8"))
        for tag in ("pl-PL", "zh-Hans"):
            nodes = translation_nodes(
                self.root / "i18n" / "app" /
                f"gamehq_{'pl_PL' if tag == 'pl-PL' else 'zh_Hans'}.ts"
            )
            self.assertEqual(
                target_before[tag]["gamehq.fixture.keep"],
                nodes["gamehq.fixture.keep"],
            )
            self.assertEqual(
                target_before[tag]["gamehq.fixture.removed"],
                nodes["gamehq.fixture.removed"],
            )
            for message_id in ("gamehq.fixture.captures", "gamehq.fixture.launch"):
                entry = state_after["locales"][tag]["messages"][message_id]
                self.assertEqual("machine_translated", entry["status"])
                self.assertEqual(
                    next(
                        unit["source_hash"]
                        for unit in workset["locales"][tag]["queue"]["units"]
                        if unit["id"] == message_id
                    ),
                    entry["source_hash"],
                )
                self.assertEqual("machine", entry["provenance"]["kind"])
                self.assertEqual(UPDATED_AT, entry["updated_at"])
            self.assertEqual(
                state_before["locales"][tag]["messages"]["gamehq.fixture.keep"],
                state_after["locales"][tag]["messages"]["gamehq.fixture.keep"],
            )

        report = json.loads(catalog_status.read_text(encoding="utf-8"))
        for tag in ("en-US", "pl-PL", "zh-Hans"):
            self.assertEqual(
                ["gamehq.fixture.removed"], report["locales"][tag]["obsolete"]
            )

        self.run_sync()
        self.run_verify()
        complete_first = self.work / "complete-first.json"
        self.run_check(base_manifest, complete_first, expected=0)
        final_snapshot = snapshot(self.root / "i18n")

        self.run_sync()
        self.run_verify()
        complete_second = self.work / "complete-second.json"
        self.run_check(base_manifest, complete_second, expected=0)
        self.assertEqual(final_snapshot, snapshot(self.root / "i18n"))
        self.assertEqual(complete_first.read_bytes(), complete_second.read_bytes())


def main() -> None:
    global LUPDATE
    parser = argparse.ArgumentParser()
    parser.add_argument("--lupdate", required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    LUPDATE = arguments.lupdate
    unittest.main(argv=[sys.argv[0], *unittest_arguments])


if __name__ == "__main__":
    main()
