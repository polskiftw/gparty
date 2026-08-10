from __future__ import annotations

import hashlib
import json
import logging
import mimetypes
import os
import shutil
import time
import uuid
from concurrent.futures import Future, ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

from .config import BoinkConfig, load_collector_settings
from .source.reddit import (
    build_gallery_dl_config,
    normalize_sources,
    run_gallery_dl,
    source_identity,
)
from .state import StateStore
from .storage.b2 import B2Transport
from .storage.r2 import R2Transport

log = logging.getLogger("boink.acquire")


class AcquisitionPaths:
    def __init__(self, root: Path):
        self.root = root
        self.archive = root / "gallery-dl-archive.sqlite3"
        self.settings = root / "settings.runtime.json"
        self.gallery_config = root / "gallery-dl.generated.json"
        self.cookies = root / "reddit-cookies.txt"
        self.downloads = root / "downloads"
        self.partials = root / "partials"
        self.run_state = root / "run-state.json"


def data_paths() -> AcquisitionPaths:
    root = Path(os.getenv("BOINK_DATA_DIR", Path.cwd() / ".boink-run"))
    root.mkdir(parents=True, exist_ok=True)
    return AcquisitionPaths(root)


def _r2_sources(r2: R2Transport, key: str) -> list[Any]:
    payload = r2.get_bytes(key)
    if payload is None:
        return []
    value = json.loads(payload)
    values = value if isinstance(value, list) else value.get("sources", [])
    if not isinstance(values, list):
        raise TypeError("R2 Reddit source configuration has an invalid shape")
    return values


def prepare_acquisition(
    config: BoinkConfig,
    b2: B2Transport,
    r2: R2Transport,
    *,
    settings_path: Path | None = None,
) -> dict[str, Any]:
    store = StateStore(b2, config)
    paths = data_paths()
    if paths.downloads.exists():
        shutil.rmtree(paths.downloads)
    if paths.partials.exists():
        shutil.rmtree(paths.partials)
    paths.downloads.mkdir(parents=True)
    paths.partials.mkdir(parents=True)

    settings = load_collector_settings(settings_path)
    sources = normalize_sources(_r2_sources(r2, config.reddit_sources_key))
    settings["sources"] = sources
    paths.settings.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")

    store.write_json(
        "acquire/source-snapshot.json",
        {"version": 1, "updated_at": int(time.time()), "sources": sources},
        purpose="private-source-snapshot",
    )
    archive_record = b2.find_object(store.key("acquire/gallery-dl-archive.sqlite3"))
    if archive_record is None:
        raise RuntimeError("Boink acquisition history is missing; refusing acquisition")
    b2.download_file(archive_record, paths.archive)
    state = {
        "version": 1,
        "run_id": f"acq-{int(time.time())}-{uuid.uuid4().hex[:8]}",
        "created_at": int(time.time()),
        "source_count": len(sources),
        "phase": "prepared",
    }
    paths.run_state.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    return state


def _require_route_flag(name: str) -> None:
    if os.getenv("GITHUB_ACTIONS", "").lower() != "true":
        return
    if os.getenv(name, "") != "1":
        raise RuntimeError(f"Required route verification flag {name} is absent")


def download_acquisition(config: BoinkConfig) -> dict[str, Any]:
    del config
    _require_route_flag("BOINK_SOURCE_ROUTE_VERIFIED")
    paths = data_paths()
    if (
        not paths.run_state.exists()
        or not paths.archive.exists()
        or not paths.settings.exists()
    ):
        raise RuntimeError("Acquisition prepare phase is incomplete")
    state = json.loads(paths.run_state.read_text(encoding="utf-8"))
    settings = json.loads(paths.settings.read_text(encoding="utf-8"))
    build_gallery_dl_config(
        settings,
        archive_path=paths.archive,
        cookies_path=paths.cookies,
        config_path=paths.gallery_config,
        partial_root=paths.partials,
    )
    source_states: list[dict[str, Any]] = []
    discovered = 0
    for number, source in enumerate(settings["sources"], start=1):
        source_root = paths.downloads / f"source-{number}"
        checkpoint = paths.root / f"archive-before-source-{number}.sqlite3"
        if paths.archive.exists():
            shutil.copy2(paths.archive, checkpoint)
        else:
            checkpoint.unlink(missing_ok=True)
        return_code = run_gallery_dl(
            str(source), source_root, settings, paths.gallery_config
        )
        count = sum(1 for item in source_root.rglob("*") if item.is_file())
        discovered += count
        source_states.append(
            {
                "number": number,
                "source_id": source_identity(str(source)),
                "return_code": return_code,
                "discovered": count,
                "source_dir": str(source_root),
                "checkpoint": str(checkpoint),
            }
        )
    if discovered == 0 and not any(item["return_code"] == 0 for item in source_states):
        raise RuntimeError(
            "Every configured source failed before staging a complete file"
        )
    state.update(
        {"phase": "downloaded", "discovered": discovered, "sources": source_states}
    )
    paths.run_state.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    return state


def safe_name(path: Path) -> str:
    clean = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in path.name)
    return clean[-160:] or "media.bin"


def canonical_object_key(prefix: str, file_path: Path, relative_path: str) -> str:
    digest = hashlib.sha1()
    digest.update(relative_path.encode("utf-8", errors="replace"))
    digest.update(b"\0")
    with file_path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return f"{prefix.strip('/')}/{digest.hexdigest()[:20]}_{safe_name(file_path)}"


def _sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _commit_one(
    b2: B2Transport,
    *,
    path: Path,
    key: str,
    size: int,
    sha1: str,
    source_id: str,
    relative_identity: str,
) -> str:
    existing = b2.find_object(key)
    if existing and existing.size == size and existing.sha1 == sha1:
        return "known"
    b2.upload_file(
        path,
        key,
        content_type=mimetypes.guess_type(path.name)[0] or "b2/x-auto",
        metadata={
            "boink-provenance": "source-acquisition",
            "boink-source-id": source_id,
            "boink-relative-id": relative_identity,
            "boink-key-version": "boink-v1",
        },
        expected_size=size,
        expected_sha1=sha1,
    )
    return "uploaded"


def commit_acquisition(config: BoinkConfig, b2: B2Transport) -> dict[str, Any]:
    _require_route_flag("BOINK_DIRECT_ROUTE_VERIFIED")
    paths = data_paths()
    state = json.loads(paths.run_state.read_text(encoding="utf-8"))
    if state.get("phase") != "downloaded":
        raise RuntimeError("Acquisition download phase is incomplete")
    settings = json.loads(paths.settings.read_text(encoding="utf-8"))
    allowed = {
        str(value).lower().lstrip(".") for value in settings["allowed_extensions"]
    }
    maximum = int(settings.get("maximum_file_size_mb", 500)) * 1024 * 1024

    candidates: list[dict[str, Any]] = []
    filtered = 0
    for source in state["sources"]:
        source_root = Path(source["source_dir"])
        for path in source_root.rglob("*"):
            if not path.is_file():
                continue
            extension = path.suffix.lower().lstrip(".")
            size = path.stat().st_size
            if extension not in allowed or size <= 0 or size > maximum:
                filtered += 1
                path.unlink(missing_ok=True)
                continue
            relative = path.relative_to(source_root).as_posix()
            sha1 = _sha1(path)
            candidates.append(
                {
                    "path": path,
                    "key": canonical_object_key(
                        config.canonical_prefix, path, relative
                    ),
                    "size": size,
                    "sha1": sha1,
                    "source_id": source["source_id"],
                    "source_number": int(source["number"]),
                    "relative_identity": hashlib.sha256(
                        relative.encode("utf-8")
                    ).hexdigest(),
                }
            )

    uploaded = 0
    known = 0
    failed = 0
    transferred_bytes = 0
    failures_by_source: set[int] = {
        int(source["number"])
        for source in state["sources"]
        if int(source["return_code"]) != 0
    }
    with ThreadPoolExecutor(
        max_workers=config.acquire_upload_workers, thread_name_prefix="boink-b2-upload"
    ) as executor:
        futures: dict[Future[str], dict[str, Any]] = {}
        for item in candidates:
            future = executor.submit(
                _commit_one,
                b2,
                path=item["path"],
                key=item["key"],
                size=item["size"],
                sha1=item["sha1"],
                source_id=item["source_id"],
                relative_identity=item["relative_identity"],
            )
            futures[future] = item
        for future in as_completed(futures):
            item = futures[future]
            try:
                result = future.result()
            except Exception:
                failed += 1
                failures_by_source.add(item["source_number"])
                log.exception("A staged asset failed permanent B2 commit")
                continue
            if result == "known":
                known += 1
            else:
                uploaded += 1
                transferred_bytes += item["size"]
            item["path"].unlink(missing_ok=True)

    if failures_by_source:
        earliest = min(failures_by_source)
        source = next(
            item for item in state["sources"] if int(item["number"]) == earliest
        )
        checkpoint = Path(source["checkpoint"])
        if checkpoint.exists():
            shutil.copy2(checkpoint, paths.archive)
        else:
            paths.archive.unlink(missing_ok=True)
    archive_key = StateStore(b2, config).key("acquire/gallery-dl-archive.sqlite3")
    if not paths.archive.exists() or paths.archive.stat().st_size <= 0:
        raise RuntimeError("Acquisition history became empty; refusing to commit it")
    archive_size, archive_sha1 = b2.hash_file(paths.archive)
    b2.upload_file(
        paths.archive,
        archive_key,
        content_type="application/x-sqlite3",
        metadata={"boink-purpose": "gallery-dl-history"},
        expected_size=archive_size,
        expected_sha1=archive_sha1,
    )

    summary = {
        "version": 1,
        "run_id": state["run_id"],
        "finished_at": int(time.time()),
        "sources_attempted": len(state["sources"]),
        "source_partial_failures": len(failures_by_source),
        "discovered": int(state.get("discovered", 0)),
        "staged": len(candidates),
        "filtered_rejected": filtered,
        "already_known": known,
        "uploaded_verified": uploaded,
        "permanently_failed": failed,
        "bytes_transferred": transferred_bytes,
        "retry_events": b2.retry_events,
        "history_rolled_back": bool(failures_by_source),
    }
    store = StateStore(b2, config)
    store.write_json(
        f"acquire/runs/{state['run_id']}.json", summary, purpose="acquisition-summary"
    )
    state.update({"phase": "committed", "summary": summary})
    paths.run_state.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    return summary
