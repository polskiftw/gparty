from __future__ import annotations

import mimetypes
import os
from typing import Any

import boto3
from botocore.client import Config

from index_store import merge_index_items, read_index_items

INDEX_KEY = os.getenv("R2_INDEX_KEY", "gallery-index.json")
GALLERY_PREFIX = os.getenv("R2_GALLERY_PREFIX", "gallery/").strip("/") + "/"
ALLOWED_EXTENSIONS = {"jpg", "jpeg", "png", "gif", "webp", "mp4", "m4v", "webm"}


def required_env(name: str) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        raise RuntimeError(f"Missing required environment variable: {name}")
    return value


def r2_client():
    account_id = required_env("R2_ACCOUNT_ID")
    return boto3.client(
        "s3",
        endpoint_url=f"https://{account_id}.r2.cloudflarestorage.com",
        aws_access_key_id=required_env("R2_ACCESS_KEY_ID"),
        aws_secret_access_key=required_env("R2_SECRET_ACCESS_KEY"),
        region_name="auto",
        config=Config(signature_version="s3v4", retries={"max_attempts": 8, "mode": "adaptive"}),
    )


def list_gallery_objects(client, bucket: str) -> list[dict[str, Any]]:
    objects: list[dict[str, Any]] = []
    paginator = client.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=bucket, Prefix=GALLERY_PREFIX):
        for item in page.get("Contents", []):
            key = str(item.get("Key", ""))
            size = int(item.get("Size", 0))
            extension = key.rsplit(".", 1)[-1].lower() if "." in key else ""
            if not key or key.endswith("/") or size <= 0 or extension not in ALLOWED_EXTENSIONS:
                continue
            objects.append({"key": key, "ext": extension, "size": size})
    return objects


def main() -> int:
    client = r2_client()
    bucket = required_env("R2_BUCKET_NAME")

    index_items = read_index_items(client, bucket, INDEX_KEY)
    indexed_keys = {
        str(item.get("key"))
        for item in index_items
        if isinstance(item, dict) and item.get("key")
    }
    gallery_objects = list_gallery_objects(client, bucket)
    missing = [item for item in gallery_objects if item["key"] not in indexed_keys]

    print(f"R2 media objects found: {len(gallery_objects)}")
    print(f"Already indexed: {len(gallery_objects) - len(missing)}")
    print(f"Missing entries found: {len(missing)}")

    if missing:
        merged_items = merge_index_items(client, bucket, INDEX_KEY, missing)
        print(f"Missing entries restored: {len(missing)}")
        print(f"Index entries after repair: {len(merged_items)}")
    else:
        print("The gallery index was already complete; no write was needed.")

    print("Objects deleted: 0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
