#!/usr/bin/env python3
"""Focused validation for the bounded translation-agent protocol."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
FIXTURE = REPOSITORY / "tests" / "fixtures" / "i18n-agent"
PROTOCOL = REPOSITORY / "tools" / "i18n" / "protocol.py"
sys.path.insert(0, str(REPOSITORY / "tools" / "i18n"))
import protocol  # noqa: E402
import sync as extraction  # noqa: E402
import verify  # noqa: E402


class ProtocolTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gamehq-protocol-test-")
        self.root = Path(self.temporary.name)
        self.queue = json.loads((FIXTURE / "queue.json").read_text(encoding="utf-8"))
        self.response = json.loads((FIXTURE / "response.json").read_text(encoding="utf-8"))

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_protocol(
        self,
        queue: dict[str, object] | None = None,
        response: dict[str, object] | None = None,
        *extra: str,
        expected: int = 0,
    ) -> subprocess.CompletedProcess[str]:
        queue_path = self.root / "queue.json"
        response_path = self.root / "response.json"
        queue_path.write_text(json.dumps(queue or self.queue, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        response_path.write_text(json.dumps(response or self.response, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        command = [
            sys.executable, str(PROTOCOL), "--root", str(REPOSITORY),
            "--queue", str(queue_path), "--response", str(response_path), *extra,
        ]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace")
        self.assertEqual(expected, result.returncode, msg=f"command: {command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        return result

    def test_valid_response_and_canonical_serialization_are_byte_stable(self) -> None:
        first = self.root / "first.json"
        second = self.root / "second.json"
        self.run_protocol(None, None, "--canonical-response", str(first))
        self.run_protocol(None, None, "--canonical-response", str(second))
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_malformed_agent_output_fails(self) -> None:
        response = copy.deepcopy(self.response)
        response["unexpected"] = True
        result = self.run_protocol(response=response, expected=2)
        self.assertIn("fields differ", result.stderr)

    def test_wrong_locale_id_and_stale_hash_fail(self) -> None:
        cases = (
            ("target_locale", "de-DE", "response.target_locale"),
            ("id", "gamehq.fixture.unrequested", "unrequested or wrong ID"),
            ("source_hash", "0" * 64, "stale source hash"),
        )
        for field, value, expected_error in cases:
            with self.subTest(field=field):
                response = copy.deepcopy(self.response)
                if field == "target_locale":
                    response[field] = value
                else:
                    response["units"][0][field] = value
                result = self.run_protocol(response=response, expected=2)
                self.assertIn(expected_error, result.stderr)

    def test_placeholder_and_protected_token_mutation_fail(self) -> None:
        cases = (
            ("Otwórz w GameHQ", "placeholder multiset"),
            ("Otwórz %1 w aplikacji", "protected token differs"),
        )
        for translation, expected_error in cases:
            with self.subTest(translation=translation):
                response = copy.deepcopy(self.response)
                response["units"][0]["translation"] = [translation]
                result = self.run_protocol(response=response, expected=2)
                self.assertIn(expected_error, result.stderr)

    def test_stale_queue_source_hash_fails_before_response_is_considered(self) -> None:
        queue = copy.deepcopy(self.queue)
        queue["units"][0]["source_hash"] = "0" * 64
        result = self.run_protocol(queue=queue, expected=2)
        self.assertIn("stale or inconsistent English source", result.stderr)

    def test_plural_and_markup_constraints_fail(self) -> None:
        queue = copy.deepcopy(self.queue)
        response = copy.deepcopy(self.response)
        unit = queue["units"][0]
        unit.update({
            "source": "<b>Open %n files in GameHQ</b>",
            "source_hash": extraction.message_source_hash(
                "<b>Open %n files in GameHQ</b>", unit["context"]
            ),
            "plural": True,
            "placeholders": ["%n"],
            "markup_signature": ["<b>", "</b>"],
        })
        response_unit = response["units"][0]
        response_unit["source_hash"] = unit["source_hash"]
        response_unit["translation"] = ["<b>Otwórz %n plików w GameHQ</b>"]
        response_unit["translation_hash"] = verify.translation_hash(response_unit["translation"])
        result = self.run_protocol(queue=queue, response=response, expected=2)
        self.assertIn("expected 3 plural forms", result.stderr)

        unit.update({
            "source": "<b>Open %1 in GameHQ</b>",
            "source_hash": extraction.message_source_hash(
                "<b>Open %1 in GameHQ</b>", unit["context"]
            ),
            "plural": False,
            "placeholders": ["%1"],
        })
        response_unit["source_hash"] = unit["source_hash"]
        response_unit["translation"] = ["<i>Otwórz %1 w GameHQ</i>"]
        response_unit["translation_hash"] = verify.translation_hash(response_unit["translation"])
        result = self.run_protocol(queue=queue, response=response, expected=2)
        self.assertIn("markup signature differs", result.stderr)

    def test_every_tier_one_locale_has_a_valid_style_guide(self) -> None:
        registry = json.loads((REPOSITORY / "i18n" / "locales.json").read_text(encoding="utf-8"))
        tier_one = sorted(
            locale["tag"] for locale in registry["locales"] if locale["tier"] == 1
        )
        self.assertEqual(12, len(tier_one))
        for tag in tier_one:
            with self.subTest(locale=tag):
                glossary, style = protocol.load_policy(REPOSITORY, tag)
                self.assertEqual(1, glossary["schema_version"])
                self.assertEqual(tag, style["locale"])


if __name__ == "__main__":
    unittest.main()
