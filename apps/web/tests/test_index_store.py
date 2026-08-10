from __future__ import annotations

import io
import json
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


SCRIPTS_DIR = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

from index_store import merge_index_items, remove_index_keys


class FakeClientError(Exception):
    def __init__(self, code: str):
        super().__init__(code)
        self.response = {"Error": {"Code": code}}


class FakeIndexClient:
    def __init__(self, items=None):
        self.items = items
        self.version = 1 if items is not None else 0
        self.before_first_put = None
        self.put_calls: list[dict] = []

    def get_object(self, **_kwargs):
        if self.items is None:
            raise FakeClientError("NoSuchKey")
        body = json.dumps({"items": self.items}).encode("utf-8")
        return {"Body": io.BytesIO(body), "ETag": f'"v{self.version}"'}

    def put_object(self, **kwargs):
        self.put_calls.append(kwargs)
        if self.before_first_put is not None:
            callback = self.before_first_put
            self.before_first_put = None
            callback()
        if kwargs.get("IfMatch") != (f"v{self.version}" if self.items is not None else None):
            raise FakeClientError("PreconditionFailed")
        if self.items is None and kwargs.get("IfNoneMatch") != "*":
            raise FakeClientError("PreconditionFailed")
        self.items = json.loads(kwargs["Body"].decode("utf-8"))["items"]
        self.version += 1


class IndexStoreTests(unittest.TestCase):
    @patch("index_store.time.sleep", return_value=None)
    def test_merge_retries_without_restoring_a_concurrent_deletion(self, _sleep) -> None:
        client = FakeIndexClient([{"key": "keep"}, {"key": "deleted"}])

        def concurrent_change() -> None:
            client.items = [{"key": "keep"}, {"key": "other-writer"}]
            client.version += 1

        client.before_first_put = concurrent_change
        updated = merge_index_items(
            client,
            "bucket",
            "gallery-index.json",
            [{"key": "new"}],
        )

        self.assertEqual(
            {item["key"] for item in updated},
            {"keep", "other-writer", "new"},
        )
        self.assertNotIn("deleted", {item["key"] for item in client.items})
        self.assertEqual(len(client.put_calls), 2)

    def test_new_index_uses_create_only_precondition(self) -> None:
        client = FakeIndexClient()
        merge_index_items(
            client,
            "bucket",
            "gallery-index.json",
            [{"key": "new"}],
        )
        self.assertEqual(client.put_calls[0]["IfNoneMatch"], "*")

    @patch("index_store.time.sleep", return_value=None)
    def test_removal_retries_against_the_latest_index(self, _sleep) -> None:
        client = FakeIndexClient([{"key": "delete"}, {"key": "keep"}])

        def concurrent_addition() -> None:
            client.items.append({"key": "concurrent"})
            client.version += 1

        client.before_first_put = concurrent_addition
        removed, updated = remove_index_keys(
            client,
            "bucket",
            "gallery-index.json",
            {"delete"},
        )
        self.assertEqual(removed, 1)
        self.assertEqual({item["key"] for item in updated}, {"keep", "concurrent"})


if __name__ == "__main__":
    unittest.main()
