#!/usr/bin/env python3
"""Focused fixtures for incremental localization closure."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
FIXTURE = REPOSITORY / "tests" / "fixtures" / "i18n-diff" / "scenario.json"
CHECK_DIFF = REPOSITORY / "tools" / "i18n" / "check_diff.py"
sys.path.insert(0, str(REPOSITORY / "tools" / "i18n"))
import check_diff  # noqa: E402
import sync as extraction  # noqa: E402
import verify  # noqa: E402


def message(value: dict[str, str]) -> dict[str, object]:
    source = value["source"]
    context = value["context"]
    message_id = value["id"]
    return {
        "id": message_id,
        "source": source,
        "context": context,
        "domain": "application",
        "locations": [{"file": "src/fixture.cpp", "line": 10}],
        "plural": "%n" in extraction.PLACEHOLDER.findall(source),
        "placeholders": sorted(set(extraction.PLACEHOLDER.findall(source))),
        "markup_signature": extraction.markup_signature(source),
        "protected_tokens": extraction.protected_tokens(source),
        "source_hash": extraction.message_source_hash(source, context),
    }


class DiffCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        cls.base = {value["id"]: message(value) for value in fixture["base"]}
        cls.current = {value["id"]: message(value) for value in fixture["current"]}
        cls.translations = fixture["translations"]

    def setUp(self) -> None:
        self.registry = {
            "source_language": "en-US",
            "locales": [
                {"tag": "en-US", "tier": 1, "state": "enabled", "direction": "ltr", "fallback": None},
                {"tag": "de-DE", "tier": 1, "state": "enabled", "direction": "ltr", "fallback": "en-US"},
                {"tag": "pl-PL", "tier": 1, "state": "enabled", "direction": "ltr", "fallback": "en-US"},
            ],
        }
        current_hash = {
            message_id: value["source_hash"] for message_id, value in self.current.items()
        }
        old_changed_hash = self.base["gamehq.fixture.changed"]["source_hash"]
        self.state = {
            "locales": {
                "de-DE": {
                    "messages": {
                        message_id: {"status": "machine_translated", "source_hash": source_hash}
                        for message_id, source_hash in current_hash.items()
                    }
                },
                "pl-PL": {
                    "messages": {
                        "gamehq.fixture.added": {"status": "missing", "source_hash": current_hash["gamehq.fixture.added"]},
                        "gamehq.fixture.changed": {"status": "stale", "source_hash": old_changed_hash},
                        "gamehq.fixture.unchanged": {"status": "human_reviewed", "source_hash": current_hash["gamehq.fixture.unchanged"]},
                    }
                },
            }
        }
        self.payloads = {
            tag: {
                message_id: {"forms": forms, "unfinished": False}
                for message_id, forms in translations.items()
            }
            for tag, translations in self.translations.items()
        }

    def workset(self) -> dict[str, object]:
        return check_diff.build_workset(
            base_label="fixture-base",
            source_manifest="fixture-current.json",
            base_messages=self.base,
            current_messages=self.current,
            registry=self.registry,
            state=self.state,
            payloads=self.payloads,
            policy_root=REPOSITORY,
        )

    def test_exact_delta_preserves_unchanged_and_obsolete_history(self) -> None:
        workset = self.workset()
        self.assertEqual(["gamehq.fixture.added"], workset["delta"]["added"])
        self.assertEqual(["gamehq.fixture.changed"], workset["delta"]["changed"])
        self.assertEqual(["gamehq.fixture.removed"], workset["delta"]["removed"])
        self.assertEqual(["gamehq.fixture.unchanged"], workset["delta"]["unchanged"])
        self.assertEqual(
            ["gamehq.fixture.added", "gamehq.fixture.changed"],
            workset["locales"]["pl-PL"]["required"],
        )
        self.assertEqual(
            ["gamehq.fixture.added", "gamehq.fixture.changed"],
            [unit["id"] for unit in workset["locales"]["pl-PL"]["queue"]["units"]],
        )
        self.assertEqual(
            ["gamehq.fixture.added", "gamehq.fixture.changed"],
            workset["locales"]["de-DE"]["complete"],
        )
        self.assertNotIn(
            "gamehq.fixture.unchanged",
            workset["locales"]["pl-PL"]["required"],
        )
        report = {
            "$schema": "fixture",
            "schema_version": 1,
            "source_manifest": "fixture-current.json",
            "locales": {
                tag: {
                    "catalog": f"i18n/app/gamehq_{tag}.ts",
                    "missing": [],
                    "stale": [],
                    "obsolete": ["gamehq.fixture.removed"],
                    "unchanged": sorted(self.current),
                }
                for tag in ("de-DE", "en-US", "pl-PL")
            },
        }
        check_diff.validate_obsolete_history(report, workset["delta"]["removed"])
        report["locales"]["pl-PL"]["obsolete"] = []
        with self.assertRaisesRegex(check_diff.DiffCheckError, "absent from obsolete history"):
            check_diff.validate_obsolete_history(report, workset["delta"]["removed"])
        self.assertFalse(check_diff.is_complete(workset))

    def test_partial_locale_completion_is_not_closed(self) -> None:
        workset = self.workset()
        self.assertEqual([], workset["locales"]["de-DE"]["required"])
        self.assertEqual(2, len(workset["locales"]["pl-PL"]["required"]))
        self.assertFalse(check_diff.is_complete(workset))

    def test_stale_response_is_rejected(self) -> None:
        workset = self.workset()
        queue = workset["locales"]["pl-PL"]["queue"]
        response_units = []
        translations = {
            "gamehq.fixture.added": ["Dodaj %1 do GameHQ"],
            "gamehq.fixture.changed": ["Uruchom %1 w GameHQ"],
        }
        for unit in queue["units"]:
            forms = translations[unit["id"]]
            response_units.append({
                "id": unit["id"],
                "source_hash": unit["source_hash"],
                "translation": forms,
                "translation_hash": verify.translation_hash(forms),
                "status": "machine_translated",
                "provenance": {"kind": "machine", "actor": "fixture"},
            })
        response_units[0]["source_hash"] = "0" * 64
        response = {
            "$schema": "fixture",
            "protocol_version": queue["protocol_version"],
            "batch_id": queue["batch_id"],
            "source_locale": queue["source_locale"],
            "target_locale": queue["target_locale"],
            "units": response_units,
        }
        with tempfile.TemporaryDirectory(prefix="gamehq-diff-response-") as temporary:
            directory = Path(temporary)
            (directory / "pl-PL.json").write_text(
                json.dumps(response, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(check_diff.DiffCheckError, "stale source hash"):
                check_diff.validate_response_directory(workset, directory)

    def test_fully_completed_delta_closes_without_touching_unchanged(self) -> None:
        current_state = copy.deepcopy(self.state)
        current_payloads = copy.deepcopy(self.payloads)
        for message_id, forms in {
            "gamehq.fixture.added": ["Dodaj %1 do GameHQ"],
            "gamehq.fixture.changed": ["Uruchom %1 w GameHQ"],
        }.items():
            current_state["locales"]["pl-PL"]["messages"][message_id] = {
                "status": "machine_translated",
                "source_hash": self.current[message_id]["source_hash"],
            }
            current_payloads["pl-PL"][message_id] = {"forms": forms, "unfinished": False}
        workset = check_diff.build_workset(
            base_label="fixture-base",
            source_manifest="fixture-current.json",
            base_messages=self.base,
            current_messages=self.current,
            registry=self.registry,
            state=current_state,
            payloads=current_payloads,
            policy_root=REPOSITORY,
        )
        self.assertTrue(check_diff.is_complete(workset))
        self.assertIsNone(workset["locales"]["pl-PL"]["queue"])
        self.assertEqual(
            ["gamehq.fixture.unchanged"], workset["delta"]["unchanged"]
        )

    def test_repeated_repository_no_delta_output_is_byte_stable_and_offline(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gamehq-diff-output-") as temporary:
            first = Path(temporary) / "first.json"
            second = Path(temporary) / "second.json"
            command = [
                sys.executable,
                str(CHECK_DIFF),
                "--root", str(REPOSITORY),
                "--base-manifest", str(REPOSITORY / "i18n" / "extracted" / "messages.json"),
            ]
            for output in (first, second):
                result = subprocess.run(
                    [*command, "--output", str(output)],
                    capture_output=True,
                    text=True,
                    errors="replace",
                )
                self.assertEqual(
                    0,
                    result.returncode,
                    msg=f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
                )
            self.assertEqual(first.read_bytes(), second.read_bytes())


if __name__ == "__main__":
    unittest.main()
