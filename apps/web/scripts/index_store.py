from __future__ import annotations

import json
import time
from collections.abc import Callable
from typing import Any


IndexItem = dict[str, Any]
IndexMutator = Callable[[list[IndexItem]], list[IndexItem]]

_MISSING_CODES = {"NoSuchKey", "404", "NotFound"}
_CONFLICT_CODES = {"PreconditionFailed", "412"}


def _error_code(exc: Exception) -> str:
    response = getattr(exc, "response", {})
    if not isinstance(response, dict):
        return ""
    error = response.get("Error", {})
    return str(error.get("Code", "")) if isinstance(error, dict) else ""


def _etag(response: dict[str, Any]) -> str:
    value = str(response.get("ETag", "")).strip().strip('"')
    if not value:
        raise RuntimeError("Gallery index response did not include an ETag")
    return value


def read_index_state(
    client,
    bucket: str,
    index_key: str,
) -> tuple[list[IndexItem], str | None]:
    try:
        response = client.get_object(Bucket=bucket, Key=index_key)
    except Exception as exc:
        if _error_code(exc) in _MISSING_CODES:
            return [], None
        raise

    payload = json.loads(response["Body"].read().decode("utf-8"))
    if isinstance(payload, dict) and isinstance(payload.get("items"), list):
        items = payload["items"]
    elif isinstance(payload, list):
        items = payload
    else:
        raise RuntimeError(f"{index_key} has an unsupported format")
    if not all(isinstance(item, dict) for item in items):
        raise RuntimeError(f"{index_key} contains an invalid item")
    return items, _etag(response)


def read_index_items(client, bucket: str, index_key: str) -> list[IndexItem]:
    items, _ = read_index_state(client, bucket, index_key)
    return items


def update_index(
    client,
    bucket: str,
    index_key: str,
    mutate: IndexMutator,
    *,
    max_attempts: int = 8,
) -> list[IndexItem]:
    """Apply a mutation to the latest index without overwriting concurrent changes."""
    for attempt in range(max_attempts):
        current, etag = read_index_state(client, bucket, index_key)
        updated = mutate(list(current))
        if updated == current:
            return updated
        payload = {
            "version": 1,
            "generated_at": int(time.time()),
            "count": len(updated),
            "items": updated,
        }
        condition = {"IfMatch": etag} if etag is not None else {"IfNoneMatch": "*"}
        try:
            client.put_object(
                Bucket=bucket,
                Key=index_key,
                Body=json.dumps(payload, separators=(",", ":")).encode("utf-8"),
                ContentType="application/json",
                CacheControl="no-cache",
                **condition,
            )
            return updated
        except Exception as exc:
            if _error_code(exc) not in _CONFLICT_CODES:
                raise
            if attempt + 1 >= max_attempts:
                break
            time.sleep(min(1.0, 0.1 * (attempt + 1)))
    raise RuntimeError("Gallery index changed repeatedly during update")


def merge_index_items(
    client,
    bucket: str,
    index_key: str,
    additions: list[IndexItem],
    *,
    max_attempts: int = 8,
) -> list[IndexItem]:
    additions_by_key = {
        str(item["key"]): item
        for item in additions
        if isinstance(item, dict) and item.get("key")
    }

    def merge(current: list[IndexItem]) -> list[IndexItem]:
        merged: dict[str, IndexItem] = {}
        for item in current:
            key = str(item.get("key", ""))
            if key:
                merged[key] = item
        merged.update(additions_by_key)
        return list(merged.values())

    return update_index(
        client,
        bucket,
        index_key,
        merge,
        max_attempts=max_attempts,
    )


def remove_index_keys(
    client,
    bucket: str,
    index_key: str,
    deleted_keys: set[str],
    *,
    max_attempts: int = 8,
) -> tuple[int, list[IndexItem]]:
    removed = 0

    def remove(current: list[IndexItem]) -> list[IndexItem]:
        nonlocal removed
        remaining = [
            item for item in current if str(item.get("key", "")) not in deleted_keys
        ]
        removed = len(current) - len(remaining)
        return remaining

    updated = update_index(
        client,
        bucket,
        index_key,
        remove,
        max_attempts=max_attempts,
    )
    return removed, updated
