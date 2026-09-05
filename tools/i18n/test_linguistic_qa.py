#!/usr/bin/env python3
"""Focused tests for contextual linguistic-review evidence."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import linguistic_qa


ROOT = Path(__file__).resolve().parents[2]
REVIEWS = ROOT / "i18n/quality/reviews"
ARTIFACT = REVIEWS / "pl-PL.json"


class LinguisticReviewTest(unittest.TestCase):
    def test_reviewed_surfaces_use_repository_lf_bytes(self) -> None:
        # Evidence hashes must survive Git's LF checkout normalization.
        # CRLF or mixed local files previously passed locally but failed CI.
        for artifact in sorted(REVIEWS.glob("*.json")):
            review = json.loads(artifact.read_text(encoding="utf-8"))
            for surface in review["surfaces"]:
                with self.subTest(locale=artifact.stem, path=surface["path"]):
                    payload = (ROOT / surface["path"]).read_bytes()
                    self.assertNotIn(b"\r", payload)

    def test_every_completed_locale_review_validates(self) -> None:
        artifacts = sorted(REVIEWS.glob("*.json"))
        self.assertTrue(artifacts)
        for artifact in artifacts:
            with self.subTest(artifact=artifact.name):
                review = linguistic_qa.validate_review(ROOT, artifact)
                self.assertEqual(artifact.stem, review["locale"])
                self.assertEqual(831, len(review["coverage"]))
                self.assertEqual([], review["unresolved"])

    def test_polish_review_has_complete_auditable_coverage(self) -> None:
        review = linguistic_qa.validate_review(ROOT, ARTIFACT)
        self.assertEqual(831, len(review["coverage"]))
        self.assertEqual(831, len({entry["id"] for entry in review["coverage"]}))
        self.assertTrue(review["corrections"])
        self.assertEqual([], review["unresolved"])
        self.assertEqual("agent", review["review"]["reviewer_kind"])
        self.assertEqual("contextually_reviewed", review["review"]["state"])

    def test_stale_hash_missing_id_and_private_evidence_are_rejected(self) -> None:
        original = json.loads(ARTIFACT.read_text(encoding="utf-8"))
        cases = []
        stale = copy.deepcopy(original)
        stale["reviewed"]["sha256"] = "0" * 64
        cases.append((stale, "catalog identity, hash, or count is stale"))
        missing = copy.deepcopy(original)
        missing["coverage"].pop()
        cases.append((missing, "coverage must contain exactly 831"))
        private = copy.deepcopy(original)
        private["review"]["telemetry"] = "forbidden"
        cases.append((private, "forbidden private field"))
        unresolved = copy.deepcopy(original)
        unresolved["unresolved"] = ["defect"]
        cases.append((unresolved, "unresolved linguistic defects"))
        for value, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic), tempfile.TemporaryDirectory() as temp:
                path = Path(temp) / "review.json"
                path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
                with self.assertRaisesRegex(linguistic_qa.ReviewError, diagnostic):
                    linguistic_qa.validate_review(ROOT, path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
