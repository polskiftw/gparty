from __future__ import annotations

import logging
import mimetypes
from concurrent.futures import Future, ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

from boto3.s3.transfer import TransferConfig

import app
from deduper.index_store import merge_index_items, read_index_items

UPLOAD_WORKERS = 8
INDEX_CHECKPOINT_SIZE = 100

# Each large object may upload several multipart chunks concurrently. Keep this
# below the outer file-worker count so one large video cannot monopolize the VM.
app.R2_TRANSFER_CONFIG = TransferConfig(
    multipart_threshold=16 * 1024 * 1024,
    multipart_chunksize=8 * 1024 * 1024,
    max_concurrency=4,
    use_threads=True,
)

log = logging.getLogger("gparty")


def remove_temporary_file(file_path: Path) -> None:
    try:
        file_path.unlink(missing_ok=True)
    except OSError:
        log.warning("Could not remove temporary file: %s", file_path)


def upload_candidate(
    client,
    *,
    file_path: Path,
    bucket: str,
    key: str,
    extension: str,
    size: int,
) -> tuple[Path, dict[str, Any]]:
    content_type = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
    app.upload_file_with_retry(
        client,
        local_path=file_path,
        bucket=bucket,
        key=key,
        extra_args={
            "ContentType": content_type,
            "CacheControl": "public, max-age=31536000, immutable",
        },
    )
    return file_path, {"key": key, "ext": extension, "size": size}


def checkpoint_index(
    client,
    bucket: str,
    pending: list[tuple[Path, dict[str, Any]]],
) -> int:
    if not pending:
        return 0

    items = [item for _, item in pending]
    merged_items = merge_index_items(client, bucket, app.INDEX_KEY, items)
    for file_path, _ in pending:
        remove_temporary_file(file_path)

    committed = len(pending)
    pending.clear()
    log.info(
        "Checkpointed %s uploaded item(s) into %s; index now contains %s media items",
        committed,
        app.INDEX_KEY,
        len(merged_items),
    )
    return committed


def upload_downloads_parallel(
    settings: dict[str, Any],
    scan_root: Path,
    client,
    bucket: str,
) -> tuple[int, int, int]:
    allowed = {str(ext).lower().lstrip(".") for ext in settings["allowed_extensions"]}
    maximum_bytes = int(settings.get("maximum_file_size_mb", 500)) * 1024 * 1024
    prefix = str(settings.get("r2_gallery_prefix", "gallery/"))
    index_items = read_index_items(client, bucket, app.INDEX_KEY)
    indexed_keys = {item.get("key") for item in index_items if isinstance(item, dict)}

    candidates: list[tuple[Path, str, str, int]] = []
    discovered = 0

    # Prepare stable object keys before starting transfers. Hashing remains local
    # and sequential so the upload pool is reserved for network work.
    for file_path in list(scan_root.rglob("*")):
        if not file_path.is_file():
            continue

        discovered += 1
        extension = file_path.suffix.lower().lstrip(".")
        if extension not in allowed:
            log.info("Discarding unwanted extension: %s", file_path.name)
            remove_temporary_file(file_path)
            continue

        size = file_path.stat().st_size
        if size <= 0 or size > maximum_bytes:
            log.info("Discarding file outside size limit: %s (%s bytes)", file_path.name, size)
            remove_temporary_file(file_path)
            continue

        relative = file_path.relative_to(scan_root).as_posix()
        key = app.object_key(prefix, file_path, relative)
        if key in indexed_keys:
            log.info("Already present in R2 index; skipping: %s", file_path.name)
            remove_temporary_file(file_path)
            continue

        # Prevent duplicate submissions inside one local batch too.
        indexed_keys.add(key)
        candidates.append((file_path, key, extension, size))

    if not candidates:
        log.info("No new media files needed uploading in this batch")
        return 0, discovered, 0

    log.info(
        "Uploading %s media file(s) with %s concurrent workers; index checkpoints every %s successes",
        len(candidates),
        UPLOAD_WORKERS,
        INDEX_CHECKPOINT_SIZE,
    )

    uploaded = 0
    failed_uploads = 0
    pending_checkpoint: list[tuple[Path, dict[str, Any]]] = []

    with ThreadPoolExecutor(max_workers=UPLOAD_WORKERS, thread_name_prefix="r2-upload") as executor:
        futures: dict[Future[tuple[Path, dict[str, Any]]], tuple[Path, str]] = {}
        for file_path, key, extension, size in candidates:
            future = executor.submit(
                upload_candidate,
                client,
                file_path=file_path,
                bucket=bucket,
                key=key,
                extension=extension,
                size=size,
            )
            futures[future] = (file_path, key)

        for future in as_completed(futures):
            file_path, key = futures[future]
            try:
                successful_path, index_item = future.result()
            except Exception:
                failed_uploads += 1
                log.exception(
                    "Could not upload %s after all retries. The scan will continue, "
                    "and this source's history will be rolled back so it can retry next run.",
                    file_path.name,
                )
                remove_temporary_file(file_path)
                continue

            pending_checkpoint.append((successful_path, index_item))
            uploaded += 1
            log.info("Uploaded %s -> r2://%s/%s", successful_path.name, bucket, key)

            if len(pending_checkpoint) >= INDEX_CHECKPOINT_SIZE:
                checkpoint_index(client, bucket, pending_checkpoint)

    checkpoint_index(client, bucket, pending_checkpoint)
    return uploaded, discovered, failed_uploads


app.upload_downloads = upload_downloads_parallel

if __name__ == "__main__":
    raise SystemExit(app.main())
