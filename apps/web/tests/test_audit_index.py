from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPTS_DIR = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

from audit_index import analyze_index


class AuditIndexTests(unittest.TestCase):
    def test_clean_index(self) -> None:
        report = analyze_index(
            {
                "count": 2,
                "items": [
                    {"key": "gallery/a.jpg", "ext": "jpg", "size": 100},
                    {"key": "gallery/b.mp4", "ext": "mp4", "size": 200},
                ],
            },
            [
                {"key": "gallery/a.jpg", "size": 100},
                {"key": "gallery/b.mp4", "size": 200},
            ],
            "gallery/",
        )
        self.assertTrue(report["clean"])
        self.assertEqual(report["unique_random_keys"], 2)
        self.assertEqual(report["duplicate_key_rows"], 0)

    def test_reports_random_weighting_and_integrity_problems(self) -> None:
        report = analyze_index(
            {
                "count": 99,
                "items": [
                    {"key": "gallery/a.jpg", "ext": "jpg", "size": 100},
                    {"key": "gallery/a.jpg", "ext": "jpg", "size": 100},
                    {"key": "gallery/missing.mp4", "ext": "jpg", "size": 0},
                    {"key": "gallery/no-extension", "size": 50},
                    {"key": "_internal/not-media.jpg", "ext": "jpg", "size": 10},
                    "bad row",
                ],
            },
            [
                {"key": "gallery/a.jpg", "size": 100},
                {"key": "gallery/unindexed.gif", "size": 300},
                {"key": "gallery/unsupported.txt", "size": 20},
            ],
            "gallery/",
        )
        self.assertFalse(report["clean"])
        self.assertEqual(report["duplicate_key_rows"], 1)
        self.assertEqual(report["indexed_keys_missing_from_r2"], 2)
        self.assertEqual(report["supported_r2_objects_missing_from_index"], 1)
        self.assertEqual(report["random_unclassified_rows"], 1)
        self.assertEqual(report["extension_mismatch_rows"], 1)
        self.assertEqual(report["invalid_size_rows"], 1)
        self.assertTrue(report["declared_count_mismatch"])


if __name__ == "__main__":
    unittest.main()
