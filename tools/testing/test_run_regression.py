"""Regression checks for evidence classification and incomplete output."""

from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
import xml.etree.ElementTree as ET

from run_regression import load_ctest_cases, load_qt_counts, report_path, result_status


class ReceiptTests(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.directory = Path(temp.name).resolve()

    def xml(self, body):
        path = self.directory / "result.xml"
        path.write_text(body, encoding="utf-8")
        return path

    def test_ctest_states_do_not_collapse_into_pass(self):
        cases = load_ctest_cases(self.xml("""
            <testsuite>
              <testcase name="green" status="run"/>
              <testcase name="red" status="fail"><failure message="Failed"/></testcase>
              <testcase name="hang" status="fail"><failure message="Timeout"/></testcase>
              <testcase name="skipped" status="notrun"><skipped message="Disabled"/></testcase>
              <testcase name="missing" status="notrun"/>
            </testsuite>
        """))
        self.assertEqual({name: case["status"] for name, case in cases.items()},
                         {"green": "passed", "red": "failed", "hang": "timeout",
                          "skipped": "skipped", "missing": "not_run"})
        self.assertEqual(result_status(8, cases, []), ("timeout", 8))

    def test_failed_native_exit_wins_over_pass_report(self):
        self.assertEqual(result_status(8, {"suite": {"status": "passed"}}, []), ("failed", 8))

    def test_zero_exit_cannot_make_missing_evidence_green(self):
        self.assertEqual(result_status(0, {}, ["No report"]), ("incomplete", 1))
        self.assertEqual(result_status(0, {"suite": {"status": "not_run"}}, []), ("incomplete", 1))

    def test_qt_skips_are_visible_even_after_a_passing_ctest_exit(self):
        counts = load_qt_counts(self.xml('<testsuite tests="4" errors="0" failures="0" skipped="2"/>'))
        self.assertEqual(counts["passed"], 2)
        self.assertEqual(result_status(0, {"suite": {"status": "passed", "qt": counts}}, []),
                         ("passed_with_skips", 0))

    def test_truncated_and_invalid_qt_reports_fail(self):
        with self.assertRaises(ET.ParseError):
            load_qt_counts(self.xml("<testsuite"))
        with self.assertRaises(ValueError):
            load_qt_counts(self.xml('<testsuite tests="1" errors="0" failures="2" skipped="0"/>'))
        with self.assertRaises(FileNotFoundError):
            load_qt_counts(self.directory / "missing.xml")

    def test_only_generated_qt_report_paths_are_used(self):
        path = self.directory / "result.xml"
        self.assertEqual(report_path({"command": [f"{path},junitxml"]}, self.directory), path)
        with self.assertRaises(ValueError):
            report_path({"command": [f"{self.directory.parent / 'outside.xml'},junitxml"]}, self.directory)


if __name__ == "__main__":
    unittest.main()
