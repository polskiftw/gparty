from __future__ import annotations

import json
import os
from collections import Counter
from typing import Any


ALLOWED_EXTENSIONS = {"jpg", "jpeg", "png", "gif", "webp", "mp4", "m4v", "webm"}
STILL_EXTENSIONS = {"jpg", "jpeg", "png", "webp"}
CLIP_EXTENSIONS = {"gif", "mp4", "m4v", "webm"}


def required_env(name: str) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        raise RuntimeError(f"Missing required environment variable: {name}")
    return value


def extension_for_key(key: str) -> str:
    return key.rsplit(".", 1)[-1].lower() if "." in key else ""


def analyze_index(
    payload: Any,
    objects: list[dict[str, Any]],
    gallery_prefix: str,
) -> dict[str, int | bool | None]:
    if isinstance(payload, dict) and isinstance(payload.get("items"), list):
        rows = payload["items"]
        declared_count = payload.get("count")
    elif isinstance(payload, list):
        rows = payload
        declared_count = None
    else:
        raise RuntimeError("gallery-index.json has an unsupported format")

    counts = Counter()
    indexed_keys: list[str] = []
    for row in rows:
        if not isinstance(row, dict):
            counts["non_object_rows"] += 1
            continue
        key = row.get("key")
        if not isinstance(key, str) or not key:
            counts["invalid_key_rows"] += 1
            continue
        if not key.startswith(gallery_prefix):
            counts["outside_gallery_prefix_rows"] += 1
            continue

        indexed_keys.append(key)
        stored_extension = str(row.get("ext", "")).lower().lstrip(".")
        actual_extension = extension_for_key(key)
        if stored_extension in STILL_EXTENSIONS:
            counts["random_still_rows"] += 1
        elif stored_extension in CLIP_EXTENSIONS:
            counts["random_clip_rows"] += 1
        else:
            counts["random_unclassified_rows"] += 1
        if stored_extension not in ALLOWED_EXTENSIONS:
            counts["unsupported_extension_rows"] += 1
        if stored_extension != actual_extension:
            counts["extension_mismatch_rows"] += 1
        size = row.get("size")
        if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
            counts["invalid_size_rows"] += 1

    unique_indexed_keys = set(indexed_keys)
    object_keys = {
        str(item["key"])
        for item in objects
        if item.get("key") and str(item["key"]).startswith(gallery_prefix)
    }
    supported_object_keys = {
        str(item["key"])
        for item in objects
        if item.get("key")
        and str(item["key"]).startswith(gallery_prefix)
        and int(item.get("size", 0)) > 0
        and extension_for_key(str(item["key"])) in ALLOWED_EXTENSIONS
    }
    unsupported_objects = object_keys - supported_object_keys

    declared_count_mismatch = (
        declared_count is not None
        and (
            isinstance(declared_count, bool)
            or not isinstance(declared_count, int)
            or declared_count != len(rows)
        )
    )
    report: dict[str, int | bool | None] = {
        "declared_count": declared_count if isinstance(declared_count, int) else None,
        "actual_index_rows": len(rows),
        "random_eligible_rows": len(indexed_keys),
        "unique_random_keys": len(unique_indexed_keys),
        "duplicate_key_rows": len(indexed_keys) - len(unique_indexed_keys),
        "random_still_rows": counts["random_still_rows"],
        "random_clip_rows": counts["random_clip_rows"],
        "random_unclassified_rows": counts["random_unclassified_rows"],
        "non_object_rows": counts["non_object_rows"],
        "invalid_key_rows": counts["invalid_key_rows"],
        "outside_gallery_prefix_rows": counts["outside_gallery_prefix_rows"],
        "unsupported_extension_rows": counts["unsupported_extension_rows"],
        "extension_mismatch_rows": counts["extension_mismatch_rows"],
        "invalid_size_rows": counts["invalid_size_rows"],
        "r2_gallery_objects": len(object_keys),
        "r2_supported_media_objects": len(supported_object_keys),
        "r2_unsupported_objects": len(unsupported_objects),
        "indexed_keys_missing_from_r2": len(unique_indexed_keys - object_keys),
        "supported_r2_objects_missing_from_index": len(
            supported_object_keys - unique_indexed_keys
        ),
        "declared_count_mismatch": declared_count_mismatch,
    }
    problem_fields = (
        "duplicate_key_rows",
        "random_unclassified_rows",
        "non_object_rows",
        "invalid_key_rows",
        "outside_gallery_prefix_rows",
        "unsupported_extension_rows",
        "extension_mismatch_rows",
        "invalid_size_rows",
        "r2_unsupported_objects",
        "indexed_keys_missing_from_r2",
        "supported_r2_objects_missing_from_index",
        "declared_count_mismatch",
    )
    report["clean"] = not any(report[field] for field in problem_fields)
    return report


def r2_client():
    import boto3
    from botocore.client import Config

    account_id = required_env("R2_ACCOUNT_ID")
    return boto3.client(
        "s3",
        endpoint_url=f"https://{account_id}.r2.cloudflarestorage.com",
        aws_access_key_id=required_env("R2_ACCESS_KEY_ID"),
        aws_secret_access_key=required_env("R2_SECRET_ACCESS_KEY"),
        region_name="auto",
        config=Config(
            signature_version="s3v4",
            retries={"max_attempts": 8, "mode": "adaptive"},
        ),
    )


def read_index_payload(client, bucket: str, index_key: str) -> Any:
    response = client.get_object(Bucket=bucket, Key=index_key)
    return json.loads(response["Body"].read().decode("utf-8"))


def list_gallery_objects(
    client,
    bucket: str,
    gallery_prefix: str,
) -> list[dict[str, Any]]:
    objects: list[dict[str, Any]] = []
    paginator = client.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=bucket, Prefix=gallery_prefix):
        for item in page.get("Contents", []):
            key = str(item.get("Key", ""))
            if key and not key.endswith("/"):
                objects.append({"key": key, "size": int(item.get("Size", 0))})
    return objects


def main() -> int:
    client = r2_client()
    bucket = required_env("R2_BUCKET_NAME")
    index_key = os.getenv("R2_INDEX_KEY", "gallery-index.json").strip()
    gallery_prefix = os.getenv("R2_GALLERY_PREFIX", "gallery/").strip("/") + "/"

    payload = read_index_payload(client, bucket, index_key)
    objects = list_gallery_objects(client, bucket, gallery_prefix)
    report = analyze_index(payload, objects, gallery_prefix)

    print("Live R2 index audit (aggregate counts only):")
    print(json.dumps(report, indent=2, sort_keys=True))
    print("AUDIT RESULT:", "CLEAN" if report["clean"] else "PROBLEMS FOUND")
    return 0 if report["clean"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
