from __future__ import annotations

import json
import unittest

from boink.acquire import _r2_sources
from boink.source.reddit import normalize_sources


class FakeR2:
    def __init__(self, payload: bytes | None):
        self.payload = payload
        self.key: str | None = None

    def get_bytes(self, key: str) -> bytes | None:
        self.key = key
        return self.payload


class RedditSourceTests(unittest.TestCase):
    def test_r2_sources_are_normalized_and_deduplicated(self) -> None:
        sources = normalize_sources(
            ["r/TestSource", "testsource", "https://old.reddit.com/r/OtherSource/"]
        )
        self.assertEqual(
            sources,
            [
                "https://www.reddit.com/r/TestSource/new/",
                "https://www.reddit.com/r/OtherSource/new/",
            ],
        )

    def test_empty_source_list_fails_closed(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "_internal/reddit-sources.json"):
            normalize_sources([])

    def test_missing_r2_object_returns_empty_list(self) -> None:
        r2 = FakeR2(None)
        self.assertEqual(_r2_sources(r2, "_internal/reddit-sources.json"), [])

    def test_r2_object_accepts_versioned_shape(self) -> None:
        payload = json.dumps({"version": 1, "sources": ["TestSource"]}).encode()
        r2 = FakeR2(payload)
        self.assertEqual(
            _r2_sources(r2, "_internal/reddit-sources.json"), ["TestSource"]
        )

    def test_malformed_r2_object_fails(self) -> None:
        r2 = FakeR2(b'{"sources":"not-a-list"}')
        with self.assertRaises(TypeError):
            _r2_sources(r2, "_internal/reddit-sources.json")


if __name__ == "__main__":
    unittest.main()
