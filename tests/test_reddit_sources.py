from __future__ import annotations

import json
import os
import unittest
from unittest.mock import patch

from boink.acquire import _managed_sources
from boink.source.reddit import merge_sources


class FakeR2:
    def __init__(self, payload: bytes | None):
        self.payload = payload

    def get_bytes(self, key: str) -> bytes | None:
        self.key = key
        return self.payload


class RedditSourceTests(unittest.TestCase):
    def test_managed_sources_are_canonical_and_normalized(self) -> None:
        managed = ["r/TestSource", "testsource", "https://old.reddit.com/r/OtherSource/"]
        with patch.dict(os.environ, {"REDDIT_SOURCE_1": "IgnoredSecret"}, clear=False):
            sources = merge_sources(["IgnoredTrackedSource"], managed)
        self.assertEqual(
            sources,
            [
                "https://www.reddit.com/r/TestSource/new/",
                "https://www.reddit.com/r/OtherSource/new/",
            ],
        )

    def test_empty_managed_source_list_fails_closed(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "_internal/reddit-sources.json"):
            merge_sources(["IgnoredTrackedSource"], [])

    def test_missing_managed_object_returns_empty_list(self) -> None:
        r2 = FakeR2(None)
        self.assertEqual(_managed_sources(r2, "_internal/reddit-sources.json"), [])

    def test_managed_object_accepts_versioned_shape(self) -> None:
        payload = json.dumps({"version": 1, "sources": ["TestSource"]}).encode()
        r2 = FakeR2(payload)
        self.assertEqual(
            _managed_sources(r2, "_internal/reddit-sources.json"), ["TestSource"]
        )

    def test_malformed_managed_object_fails(self) -> None:
        r2 = FakeR2(b'{"sources":"not-a-list"}')
        with self.assertRaises(TypeError):
            _managed_sources(r2, "_internal/reddit-sources.json")


if __name__ == "__main__":
    unittest.main()
