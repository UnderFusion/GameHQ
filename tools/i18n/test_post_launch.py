#!/usr/bin/env python3
"""Regression boundaries for explicit post-launch CI selection."""
import copy
import unittest
from unittest.mock import patch
import post_launch
import release_readiness as launch


class PostLaunchTest(unittest.TestCase):
    def setUp(self):
        self.manifest = {
            'localization_launch': {'version': '0.7.7', 'date': '2026-09-07',
                                   'status': 'released', 'designated_by': 'owner',
                                   'localization_policy': 'complete'},
            'releases': [{'version': '0.7.25'}]}

    def test_later_version_passes_without_mutation(self):
        before = copy.deepcopy(self.manifest)
        post_launch.validate_version('0.7.25', self.manifest)
        self.assertEqual(before, self.manifest)

    def test_launch_and_earlier_versions_cannot_bypass_launch_gate(self):
        for version in ('0.7.7', '0.7.6', 'broken'):
            with self.subTest(version=version), self.assertRaises(ValueError):
                post_launch.validate_version(version, self.manifest)

    def test_unfinished_or_weakened_launch_is_rejected(self):
        for field, value in (('status', 'designated'), ('date', None),
                             ('designated_by', 'agent'), ('localization_policy', 'fallback-allowed')):
            manifest = copy.deepcopy(self.manifest)
            manifest['localization_launch'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                post_launch.validate_version('0.7.25', manifest)

    def test_missing_stale_and_duplicate_history_are_rejected(self):
        for releases in ([], [{'version': '0.7.24'}], [{'version': '0.7.25'}] * 2):
            self.manifest['releases'] = releases
            with self.subTest(releases=releases), self.assertRaises(ValueError):
                post_launch.validate_version('0.7.25', self.manifest)

    def test_final_launch_mode_still_rejects_a_later_version(self):
        with patch.object(launch, 'repository_version', return_value='0.7.25'):
            with self.assertRaisesRegex(launch.ReadinessError, 'differs from the released launch'):
                launch.final_state(post_launch.ROOT, self.manifest, self.manifest['localization_launch'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
