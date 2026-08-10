from __future__ import annotations

import tempfile
import time
from pathlib import Path

from .acquire import _managed_sources
from .config import BoinkConfig, load_collector_settings
from .source.reddit import merge_sources
from .state import StateStore
from .storage.b2 import B2Transport
from .storage.r2 import R2Transport


def migrate_yoink_state(
    config: BoinkConfig,
    b2: B2Transport,
    r2: R2Transport,
    *,
    settings_path: Path | None = None,
) -> dict:
    """Idempotently copy the production Yoink history/state into Boink's B2 namespace."""
    store = StateStore(b2, config)
    source_head = r2.head(config.yoink_archive_key)
    if source_head is None or int(source_head.get("ContentLength", 0)) <= 0:
        raise RuntimeError("Current Yoink gallery-dl history is missing from R2")
    source_size = int(source_head["ContentLength"])
    source_etag = str(source_head.get("ETag", "")).strip('"')
    archive_key = store.key("acquire/gallery-dl-archive.sqlite3")
    with tempfile.TemporaryDirectory(prefix="boink-migrate-") as directory:
        local = r2.download_file(
            config.yoink_archive_key,
            Path(directory) / "gallery-dl-archive.sqlite3",
            expected_size=source_size,
        )
        actual_size, actual_sha1 = b2.hash_file(local)
        after_head = r2.head(config.yoink_archive_key)
        after_etag = str((after_head or {}).get("ETag", "")).strip('"')
        if (
            after_head is None
            or int(after_head.get("ContentLength", -1)) != source_size
            or (source_etag and after_etag != source_etag)
        ):
            raise RuntimeError(
                "Yoink history changed during migration; retry from a stable source"
            )
        existing = b2.find_object(archive_key)
        if not existing or existing.size != actual_size or existing.sha1 != actual_sha1:
            b2.upload_file(
                local,
                archive_key,
                content_type="application/x-sqlite3",
                metadata={
                    "boink-purpose": "gallery-dl-history",
                    "boink-migrated-from": "r2-yoink",
                },
                expected_size=actual_size,
                expected_sha1=actual_sha1,
            )
        verified = b2.find_object(archive_key)
        if not verified or verified.size != actual_size or verified.sha1 != actual_sha1:
            raise RuntimeError("Migrated Yoink archive failed B2 verification")

    settings = load_collector_settings(settings_path)
    managed = _managed_sources(r2, config.r2_source_config_key)
    sources = merge_sources(list(settings["sources"]), managed)
    store.write_json(
        "acquire/source-snapshot.json",
        {
            "version": 1,
            "migrated_at": int(time.time()),
            "sources": sources,
        },
        purpose="private-source-snapshot",
    )
    snapshot = store.read_json("acquire/source-snapshot.json")
    if not isinstance(snapshot, dict) or snapshot.get("sources") != sources:
        raise RuntimeError("Migrated private source snapshot failed verification")

    marker = {
        "version": 1,
        "status": "complete",
        "completed_at": int(time.time()),
        "yoink_archive_key": config.yoink_archive_key,
        "boink_archive_key": archive_key,
        "archive_size": actual_size,
        "archive_sha1": actual_sha1,
        "source_count": len(sources),
    }
    store.write_json("migration/yoink-v1.json", marker, purpose="migration-marker")
    observed = store.read_json("migration/yoink-v1.json")
    if observed != marker:
        raise RuntimeError("Yoink migration marker failed durable verification")
    return marker
