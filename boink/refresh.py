from __future__ import annotations

import base64
import hashlib
import json
import logging
import os
import secrets
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any

from .config import BoinkConfig
from .manifests import build_gallery_index, build_manifest
from .models import ManifestEntry, ObjectRecord
from .selection import byte_balanced_shards, select_random_budget, unfinished_entries
from .state import StateStore
from .storage.b2 import B2Error, B2Transport
from .storage.r2 import R2Transport

log = logging.getLogger("boink.refresh")


TERMINAL_STATES = {"active", "failed", "abandoned", "obsolete", "cleaned"}
TAG_INDEX_KEY = "_internal/tag-index-v1.json"


def _generation_id() -> str:
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + uuid.uuid4().hex[:10]


def _index_items(payload: bytes | None) -> list[dict[str, Any]]:
    if payload is None:
        return []
    value = json.loads(payload)
    items = value if isinstance(value, list) else value.get("items", [])
    if not isinstance(items, list):
        raise TypeError("R2 gallery index has an invalid shape")
    valid: list[dict[str, Any]] = []
    for item in items:
        if not isinstance(item, dict) or not str(item.get("key", "")).startswith(
            "gallery/"
        ):
            raise RuntimeError("R2 gallery index contains an invalid media row")
        valid.append(item)
    return valid




def _tag_index_for_generation(
    payload: bytes | None,
    manifest,
    *,
    previous_manifest=None,
) -> dict[str, Any] | None:
    """Remap the viewer tag index from canonical identities to this generation.

    The legacy tag index used canonical gallery/* keys. After Boink activates, the
    live tag index uses generation keys. The previous durable manifest lets us
    translate those generation keys back to canonical B2 identities before
    mapping the selected subset forward again.
    """
    if payload is None:
        return None
    value = json.loads(payload)
    if not isinstance(value, dict):
        raise RuntimeError("R2 tag index has an invalid shape")
    raw_items = value.get("items", [])
    catalog = value.get("catalog", [])
    if not isinstance(raw_items, list) or not isinstance(catalog, list):
        raise RuntimeError("R2 tag index has an invalid shape")

    prior_to_source: dict[str, str] = {}
    if previous_manifest is not None:
        prior_to_source = {
            entry.destination_key: entry.source_key for entry in previous_manifest.entries
        }
    tags_by_source: dict[str, tuple[str, list[int]]] = {}
    for row in raw_items:
        if not (isinstance(row, list) and len(row) == 3 and isinstance(row[0], str)):
            continue
        source_key = prior_to_source.get(row[0], row[0])
        if not source_key.startswith("gallery/") or not isinstance(row[2], list):
            continue
        tags_by_source[source_key] = (str(row[1]), list(row[2]))

    remapped = []
    for entry in sorted(manifest.entries, key=lambda item: item.destination_key):
        tagged = tags_by_source.get(entry.source_key)
        if tagged is None:
            continue
        _old_ext, tag_ids = tagged
        remapped.append([entry.destination_key, entry.extension, tag_ids])

    return {
        **value,
        "version": 1,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(manifest.created_at)),
        "catalog": catalog,
        "items": remapped,
    }

def active_r2_keys(r2: R2Transport, config: BoinkConfig) -> set[str]:
    return {
        str(item["key"]) for item in _index_items(r2.get_bytes(config.r2_index_key))
    }


def retry_cleanup_backlog(
    store: StateStore, r2: R2Transport, config: BoinkConfig
) -> dict[str, int]:
    backlog = store.cleanup_backlog()
    keys = {str(key) for key in backlog.get("keys", [])}
    if keys and int(backlog.get("not_before", 0)) > int(time.time()):
        return {"attempted": 0, "failed": 0, "deferred": len(keys)}
    active = active_r2_keys(r2, config)
    eligible = sorted(keys - active)
    failed = r2.delete_keys(eligible, protected=active)
    # Anything that became active is intentionally removed from deletion intent.
    store.set_cleanup_backlog(failed, reason="retry-incomplete" if failed else "clear")
    return {"attempted": len(eligible), "failed": len(failed)}


def cleanup_abandoned_generations(
    store: StateStore,
    r2: R2Transport,
    config: BoinkConfig,
    *,
    preserve_generation: str = "",
) -> dict[str, int]:
    now = int(time.time())
    current = store.current() or {}
    active_generation = str(current.get("generation", ""))
    removed = 0
    failed_keys: list[str] = []
    for state in store.known_generation_states():
        generation = str(state.get("generation", ""))
        if not generation or generation in {active_generation, preserve_generation}:
            continue
        status = str(state.get("status", ""))
        age = now - int(state.get("updated_at", state.get("created_at", now)))
        abandoned = status in {"failed", "abandoned"} or (
            status in {"building", "recovering", "ready"}
            and age >= config.abandoned_after_seconds
        )
        if not abandoned:
            continue
        keys = [
            item["key"]
            for item in r2.list_keys(f"{config.r2_generation_prefix}{generation}/")
        ]
        failures = r2.delete_keys(keys)
        removed += len(keys) - len(failures)
        failed_keys.extend(failures)
        store.write_generation_state(
            generation,
            {
                **state,
                "status": "abandoned" if failures else "cleaned",
                "cleanup_remaining": len(failures),
            },
        )
    if failed_keys:
        existing_backlog = store.cleanup_backlog()
        existing = existing_backlog.get("keys", [])
        store.set_cleanup_backlog(
            [*existing, *failed_keys],
            reason="abandoned-generation-cleanup",
            not_before=int(existing_backlog.get("not_before", 0)),
        )
    return {"removed": removed, "failed": len(failed_keys)}


def _resume_candidate(store: StateStore) -> str:
    value = store.read_json("refresh/staging.json")
    if not isinstance(value, dict):
        return ""
    generation = str(value.get("generation", ""))
    state = store.read_generation_state(generation) if generation else None
    if state and state.get("status") in {"building", "recovering", "ready"}:
        return generation
    return ""


def prepare_refresh(
    config: BoinkConfig,
    b2: B2Transport,
    r2: R2Transport,
    *,
    target_bytes: int | None = None,
    seed: str | None = None,
) -> dict[str, Any]:
    store = StateStore(b2, config)
    retry_result = retry_cleanup_backlog(store, r2, config)
    resume = _resume_candidate(store)
    coordinator_owner = os.getenv("GITHUB_RUN_ID", "").strip() or None
    if resume:
        manifest = store.read_manifest(resume)
        state = store.read_generation_state(resume) or {}
        lock = store.acquire_lock(
            "refresh",
            ttl_seconds=max(86_400, config.job_budget_seconds * 5),
            owner=coordinator_owner,
        )
        if state.get("lock_owner") != lock.owner:
            state = {**state, "lock_owner": lock.owner, "status": "building"}
            store.write_generation_state(resume, state)
        return {
            "generation": resume,
            "resumed": True,
            "selected_objects": len(manifest.entries),
            "selected_bytes": manifest.selected_bytes,
            "lock_owner": lock.owner,
            "cleanup_retry": retry_result,
            "worker_matrix": {"shard": list(range(config.refresh_workers))},
        }

    lock = store.acquire_lock(
        "refresh",
        ttl_seconds=max(86_400, config.job_budget_seconds * 5),
        owner=coordinator_owner,
    )
    generation = _generation_id()
    try:
        cleanup_abandoned_generations(store, r2, config, preserve_generation=generation)
        inventory = [
            item
            for item in b2.list_objects(config.canonical_prefix)
            if item.key.startswith(config.canonical_prefix)
            and not item.key.startswith(config.internal_prefix)
        ]
        if not inventory:
            raise RuntimeError("Canonical B2 gallery inventory is empty")
        unverifiable = [item for item in inventory if len(item.sha1) != 40]
        if unverifiable:
            raise RuntimeError(
                f"Canonical B2 inventory contains {len(unverifiable)} object(s) without a verifiable SHA-1"
            )
        selection_seed = seed or secrets.token_hex(16)
        budget = int(target_bytes or config.target_bytes)
        selected = select_random_budget(inventory, budget, selection_seed)
        if not selected:
            raise RuntimeError("No eligible canonical B2 objects were selected")
        manifest = build_manifest(
            generation=generation,
            seed=selection_seed,
            target_bytes=budget,
            inventory=inventory,
            selected=selected,
        )
        plans = byte_balanced_shards(
            manifest.entries, config.refresh_workers, generation, 0
        )
        store.write_manifest(manifest)
        store.write_shards(plans)
        state = {
            "status": "building",
            "created_at": int(time.time()),
            "round": 0,
            "lock_owner": lock.owner,
            "inventory_objects": len(inventory),
            "inventory_bytes": sum(item.size for item in inventory),
            "selected_objects": len(manifest.entries),
            "selected_bytes": manifest.selected_bytes,
            "shards": [
                {"shard": plan.shard, "objects": len(plan.entries), "bytes": plan.bytes}
                for plan in plans
            ],
        }
        store.write_generation_state(generation, state)
        store.write_json(
            "refresh/staging.json",
            {"version": 1, "generation": generation, "status": "building"},
            purpose="staging-pointer",
        )
        return {
            "generation": generation,
            "resumed": False,
            "selected_objects": len(manifest.entries),
            "selected_bytes": manifest.selected_bytes,
            "inventory_objects": len(inventory),
            "inventory_bytes": sum(item.size for item in inventory),
            "lock_owner": lock.owner,
            "cleanup_retry": retry_result,
            "shards": state["shards"],
            "worker_matrix": {"shard": list(range(config.refresh_workers))},
        }
    except Exception:
        lock.release()
        raise


def _entry_record(entry: ManifestEntry) -> ObjectRecord:
    return ObjectRecord(
        key=entry.source_key,
        size=entry.size,
        sha1=entry.sha1,
        file_id=entry.file_id,
        content_type=entry.content_type,
    )


def run_refresh_worker(
    config: BoinkConfig,
    b2: B2Transport,
    r2: R2Transport,
    *,
    generation: str,
    round_number: int,
    shard: int,
    budget_seconds: int | None = None,
) -> dict[str, Any]:
    store = StateStore(b2, config)
    manifest = store.read_manifest(generation)
    plan = store.read_shard(generation, round_number, shard)
    allowed = {item.source_key for item in manifest.entries}
    if any(item.source_key not in allowed for item in plan.entries):
        raise RuntimeError("Shard contains an object outside the durable manifest")
    prior = store.read_json(store.progress_suffix(generation, round_number, shard))
    completed = (
        {str(key) for key in prior.get("completed", [])}
        if isinstance(prior, dict)
        else set()
    )
    failed: dict[str, str] = {}
    deadline = time.monotonic() + int(budget_seconds or config.job_budget_seconds)
    uploaded = 0
    recognized = 0
    transferred = 0
    deadline_reached = False

    for index, entry in enumerate(plan.entries, start=1):
        if entry.source_key in completed:
            continue
        if time.monotonic() >= deadline:
            deadline_reached = True
            break
        if r2.verify_object(
            entry.destination_key,
            expected_size=entry.size,
            expected_sha1=entry.sha1,
            generation=generation,
        ):
            completed.add(entry.source_key)
            recognized += 1
            continue
        try:
            with tempfile.TemporaryDirectory(
                prefix=f"boink-refresh-{round_number}-{shard}-"
            ) as directory:
                local = b2.download_file(
                    _entry_record(entry), Path(directory) / "asset.ready"
                )
                r2.upload_file(
                    local,
                    entry.destination_key,
                    expected_size=entry.size,
                    expected_sha1=entry.sha1,
                    generation=generation,
                    source_identity=entry.identity,
                    content_type=entry.content_type,
                )
            completed.add(entry.source_key)
            failed.pop(entry.source_key, None)
            uploaded += 1
            transferred += entry.size
        except Exception as exc:
            failed[entry.source_key] = type(exc).__name__
            log.exception(
                "Refresh worker %s/%s could not complete one manifest object",
                round_number,
                shard,
            )
        if index % 25 == 0:
            store.write_progress(
                generation,
                round_number,
                shard,
                completed=completed,
                failed=failed,
                deadline_reached=False,
            )

    store.write_progress(
        generation,
        round_number,
        shard,
        completed=completed,
        failed=failed,
        deadline_reached=deadline_reached,
    )
    return {
        "generation": generation,
        "round": round_number,
        "shard": shard,
        "planned_objects": len(plan.entries),
        "planned_bytes": plan.bytes,
        "completed_objects": len(
            completed & {item.source_key for item in plan.entries}
        ),
        "uploaded_objects": uploaded,
        "recognized_objects": recognized,
        "failed_objects": len(failed),
        "bytes_transferred": transferred,
        "deadline_reached": deadline_reached,
        "b2_retries": b2.retry_events,
        "r2_retries": r2.retry_events,
    }


def plan_recovery_round(
    config: BoinkConfig,
    b2: B2Transport,
    r2: R2Transport,
    *,
    generation: str,
    round_number: int,
) -> dict[str, Any]:
    if round_number < 1 or round_number > config.recovery_rounds:
        raise ValueError("Recovery round is outside the configured bounded budget")
    store = StateStore(b2, config)
    manifest = store.read_manifest(generation)
    completed = store.read_completed(generation, round_number - 1)
    possible = unfinished_entries(manifest.entries, completed)
    missing: list[ManifestEntry] = []
    recognized = 0
    for entry in possible:
        if r2.verify_object(
            entry.destination_key,
            expected_size=entry.size,
            expected_sha1=entry.sha1,
            generation=generation,
        ):
            recognized += 1
        else:
            missing.append(entry)
    plans = byte_balanced_shards(
        missing, config.refresh_workers, generation, round_number
    )
    store.write_shards(plans)
    prior = store.read_generation_state(generation) or {}
    store.write_generation_state(
        generation,
        {
            **prior,
            "status": "ready" if not missing else "recovering",
            "round": round_number,
            "recovery_recognized": recognized,
            "recovery_remaining": len(missing),
            "shards": [
                {"shard": plan.shard, "objects": len(plan.entries), "bytes": plan.bytes}
                for plan in plans
            ],
        },
    )
    return {
        "generation": generation,
        "round": round_number,
        "completed_from_reports": len(completed),
        "recognized_by_head": recognized,
        "remaining_objects": len(missing),
        "remaining_bytes": sum(item.size for item in missing),
        "shards": [
            {"shard": plan.shard, "objects": len(plan.entries), "bytes": plan.bytes}
            for plan in plans
        ],
    }


def _release_refresh_lock(store: StateStore, owner: str) -> None:
    current = store.read_json("locks/refresh.json")
    if isinstance(current, dict) and current.get("owner") == owner:
        current.update({"status": "released", "released_at": int(time.time())})
        store.write_json("locks/refresh.json", current, purpose="lock")


def _rollback_json_object(
    r2: R2Transport, key: str, old_payload: bytes | None
) -> None:
    if old_payload is None:
        failures = r2.delete_keys([key])
        if failures:
            raise RuntimeError(f"Could not remove failed first publication for {key}")
        return
    r2.put_bytes(
        key,
        old_payload,
        content_type="application/json; charset=utf-8",
        cache_control="no-store",
        metadata={"boink-rollback": "true"},
        verify_body=True,
    )


def finalize_refresh(
    config: BoinkConfig,
    b2: B2Transport,
    r2: R2Transport,
    *,
    generation: str,
) -> dict[str, Any]:
    store = StateStore(b2, config)
    manifest = store.read_manifest(generation)
    state = store.read_generation_state(generation) or {}
    lock_owner = str(state.get("lock_owner", ""))
    existing_journal = store.read_json(
        f"refresh/generations/{generation}/publication.json"
    )
    already_active = (
        state.get("status") == "active"
        and (store.current() or {}).get("generation") == generation
        and isinstance(existing_journal, dict)
        and existing_journal.get("status") == "complete"
    )
    lock_state = store.read_json("locks/refresh.json")
    if not already_active and not (
        isinstance(lock_state, dict)
        and lock_state.get("status") == "held"
        and lock_state.get("owner") == lock_owner
        and int(lock_state.get("expires_at", 0)) > int(time.time())
    ):
        raise RuntimeError(
            "Refresh coordinator lock is absent, stale, or owned by another run"
        )
    missing: list[str] = []
    for entry in manifest.entries:
        if not r2.verify_object(
            entry.destination_key,
            expected_size=entry.size,
            expected_sha1=entry.sha1,
            generation=generation,
        ):
            missing.append(entry.source_key)
    if missing:
        store.write_generation_state(
            generation,
            {
                **state,
                "status": "failed",
                "failure": "recovery-budget-exhausted",
                "missing_objects": len(missing),
            },
        )
        store.write_json(
            "refresh/staging.json",
            {"version": 1, "generation": generation, "status": "failed"},
            purpose="staging-pointer",
        )
        _release_refresh_lock(store, lock_owner)
        raise RuntimeError(
            f"Refresh recovery budget exhausted with {len(missing)} object(s) unfinished; publication refused"
        )

    index = build_gallery_index(manifest, generated_at=manifest.created_at)
    expected_payload = (json.dumps(index, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )
    publication_suffix = f"refresh/generations/{generation}/publication.json"
    journal = store.read_json(publication_suffix)
    if not isinstance(journal, dict):
        captured_old_payload = r2.get_bytes(config.r2_index_key)
        captured_old_tag_payload = r2.get_bytes(TAG_INDEX_KEY)
        captured_old_current = store.current()
        journal = {
            "version": 1,
            "generation": generation,
            "status": "prepared",
            "created_at": int(time.time()),
            "new_index_sha256": hashlib.sha256(expected_payload).hexdigest(),
            "old_index_present": captured_old_payload is not None,
            "old_index_base64": base64.b64encode(captured_old_payload or b"").decode(
                "ascii"
            ),
            "old_tag_index_present": captured_old_tag_payload is not None,
            "old_tag_index_base64": base64.b64encode(captured_old_tag_payload or b"").decode("ascii"),
            "old_current": captured_old_current,
        }
        store.write_json(publication_suffix, journal, purpose="publication-journal")
        if store.read_json(publication_suffix) != journal:
            raise RuntimeError("Durable publication journal verification failed")
    old_payload = (
        base64.b64decode(str(journal.get("old_index_base64", "")), validate=True)
        if journal.get("old_index_present")
        else None
    )
    old_tag_payload = (
        base64.b64decode(str(journal.get("old_tag_index_base64", "")), validate=True)
        if journal.get("old_tag_index_present")
        else None
    )
    old_items = _index_items(old_payload)
    old_current = journal.get("old_current")
    previous_manifest = None
    if isinstance(old_current, dict):
        previous_generation = str(old_current.get("generation", ""))
        if previous_generation and previous_generation != generation:
            try:
                previous_manifest = store.read_manifest(previous_generation)
            except (TypeError, ValueError, RuntimeError):
                previous_manifest = None
    tag_index = _tag_index_for_generation(
        old_tag_payload, manifest, previous_manifest=previous_manifest
    )
    expected_tag_payload = (
        (json.dumps(tag_index, indent=2, sort_keys=True) + "\n").encode("utf-8")
        if tag_index is not None
        else None
    )
    published_payload: bytes | None = None
    try:
        if tag_index is not None:
            live_tag_payload = r2.get_bytes(TAG_INDEX_KEY)
            if live_tag_payload == expected_tag_payload:
                pass
            elif live_tag_payload == old_tag_payload:
                r2.put_json(TAG_INDEX_KEY, tag_index, generation=generation, verify_body=True)
            else:
                raise RuntimeError("Tag index changed outside this refresh; publication refused")
            if r2.get_bytes(TAG_INDEX_KEY) != expected_tag_payload:
                raise RuntimeError("Tag index publication verification failed")

        live_payload = r2.get_bytes(config.r2_index_key)
        if live_payload == expected_payload:
            published_payload = expected_payload
        elif live_payload == old_payload:
            published_payload = r2.put_json(
                config.r2_index_key, index, generation=generation, verify_body=True
            )
        else:
            raise RuntimeError(
                "Active index changed outside this refresh; publication refused"
            )
        # Body equality prevents accepting a correct-size but wrong index/pointer.
        if r2.get_bytes(config.r2_index_key) != published_payload:
            raise RuntimeError("Active gallery index publication verification failed")
        current = {
            "version": 1,
            "generation": generation,
            "activated_at": int(time.time()),
            "manifest_key": store.key(store.manifest_suffix(generation)),
            "index_sha256": hashlib.sha256(published_payload).hexdigest(),
            "objects": len(manifest.entries),
            "bytes": manifest.selected_bytes,
        }
        store.set_current(current)
        if store.current() != current:
            raise RuntimeError("Durable active-generation pointer verification failed")
        journal = {**journal, "status": "published", "published_at": int(time.time())}
        store.write_json(publication_suffix, journal, purpose="publication-journal")
    except Exception:
        try:
            _rollback_json_object(r2, config.r2_index_key, old_payload)
            _rollback_json_object(r2, TAG_INDEX_KEY, old_tag_payload)
        except Exception as rollback_error:
            store.write_generation_state(
                generation,
                {**state, "status": "publication-rollback-failed"},
            )
            _release_refresh_lock(store, lock_owner)
            raise RuntimeError(
                "Publication failed and known-good index rollback also failed"
            ) from rollback_error
        try:
            if isinstance(old_current, dict):
                store.set_current(old_current)
            else:
                store.set_current(
                    {
                        "version": 1,
                        "generation": "",
                        "status": "legacy-restored",
                        "restored_at": int(time.time()),
                    }
                )
        except (B2Error, RuntimeError, TypeError, ValueError) as pointer_error:
            # R2 has already been restored. The durable pointer can be repaired on
            # the next coordinator pass without exposing an incomplete gallery.
            log.warning(
                "The restored gallery is safe, but its durable pointer needs repair: %s",
                pointer_error,
            )
        store.write_generation_state(
            generation,
            {**state, "status": "publication-failed", "old_gallery_restored": True},
        )
        _release_refresh_lock(store, lock_owner)
        raise

    new_keys = {entry.destination_key for entry in manifest.entries}
    obsolete_keys = sorted(
        {str(item["key"]) for item in old_items if str(item["key"]) not in new_keys}
    )
    prior_backlog = [str(key) for key in store.cleanup_backlog().get("keys", [])]
    cleanup_plan = sorted(set(prior_backlog) | set(obsolete_keys))
    # Persist intent before deleting anything so a job death after publication
    # cannot lose the old-generation cleanup plan.
    store.set_cleanup_backlog(
        cleanup_plan,
        reason="published-cleanup-planned",
        not_before=int(time.time()) + config.publication_cleanup_grace_seconds,
    )
    if cleanup_plan and config.publication_cleanup_grace_seconds:
        # The current Worker may retain the previous index in memory for 60 seconds.
        # Keep its referenced objects alive beyond that cache window.
        time.sleep(config.publication_cleanup_grace_seconds)
    cleanup_failures = r2.delete_keys(cleanup_plan, protected=new_keys)
    store.set_cleanup_backlog(
        cleanup_failures,
        reason="obsolete-generation-cleanup" if cleanup_failures else "clear",
    )
    if old_current and old_current.get("generation") != generation:
        previous_generation = str(old_current.get("generation", ""))
        previous_state = store.read_generation_state(previous_generation) or {}
        store.write_generation_state(
            previous_generation,
            {
                **previous_state,
                "status": "obsolete-cleanup-pending"
                if cleanup_failures
                else "obsolete",
                "superseded_by": generation,
            },
        )

    final_state = {
        **state,
        "status": "active",
        "activated_at": int(time.time()),
        "verified_objects": len(manifest.entries),
        "verified_bytes": manifest.selected_bytes,
        "publication_verified": True,
        "cleanup_attempted": len(obsolete_keys),
        "cleanup_failed": len(cleanup_failures),
    }
    store.write_generation_state(generation, final_state)
    store.write_json(
        publication_suffix,
        {**journal, "status": "complete", "cleanup_remaining": len(cleanup_failures)},
        purpose="publication-journal",
    )
    store.write_json(
        "refresh/staging.json",
        {"version": 1, "generation": generation, "status": "complete"},
        purpose="staging-pointer",
    )
    abandoned = cleanup_abandoned_generations(
        store, r2, config, preserve_generation=generation
    )
    _release_refresh_lock(store, lock_owner)
    return {
        "generation": generation,
        "published": True,
        "objects": len(manifest.entries),
        "bytes": manifest.selected_bytes,
        "cleanup_attempted": len(obsolete_keys),
        "cleanup_backlog": len(cleanup_failures) + abandoned["failed"],
        "abandoned_cleanup": abandoned,
        "b2_retries": b2.retry_events,
        "r2_retries": r2.retry_events,
    }
