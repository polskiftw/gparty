from __future__ import annotations

import mimetypes
import time
from pathlib import PurePosixPath

from .models import ManifestEntry, ObjectRecord, RefreshManifest


def generation_destination(source_key: str, generation: str) -> str:
    prefix = "gallery/"
    if not source_key.startswith(prefix):
        raise ValueError("Canonical source key must start with gallery/")
    suffix = source_key[len(prefix) :]
    if not suffix:
        raise ValueError("Canonical source key has no filename")
    return f"gallery/generations/{generation}/{suffix}"


def build_manifest(
    *,
    generation: str,
    seed: str,
    target_bytes: int,
    inventory: list[ObjectRecord],
    selected: list[ObjectRecord],
    created_at: int | None = None,
) -> RefreshManifest:
    entries = []
    for item in selected:
        content_type = item.content_type
        if not content_type or content_type == "b2/x-auto":
            content_type = (
                mimetypes.guess_type(PurePosixPath(item.key).name)[0]
                or "application/octet-stream"
            )
        entries.append(
            ManifestEntry(
                source_key=item.key,
                destination_key=generation_destination(item.key, generation),
                size=item.size,
                sha1=item.sha1,
                file_id=item.file_id,
                extension=item.extension,
                content_type=content_type,
            )
        )
    manifest = RefreshManifest(
        generation=generation,
        created_at=created_at or int(time.time()),
        seed=seed,
        target_bytes=target_bytes,
        inventory_objects=len(inventory),
        inventory_bytes=sum(item.size for item in inventory),
        entries=entries,
    )
    manifest.validate()
    return manifest


def build_gallery_index(
    manifest: RefreshManifest, generated_at: int | None = None
) -> dict:
    manifest.validate()
    items = [
        {"key": item.destination_key, "ext": item.extension, "size": item.size}
        for item in sorted(manifest.entries, key=lambda value: value.destination_key)
    ]
    return {
        "version": 1,
        "generated_at": generated_at or int(time.time()),
        "count": len(items),
        "items": items,
    }
