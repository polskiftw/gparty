from __future__ import annotations

import hashlib
import time
import uuid
from dataclasses import dataclass
from typing import Any, Self

from .config import BoinkConfig
from .models import RefreshManifest, ShardPlan
from .storage.b2 import B2Transport


@dataclass
class DurableLock:
    store: StateStore
    name: str
    owner: str
    expires_at: int
    released: bool = False

    def release(self) -> None:
        if self.released:
            return
        current = self.store.read_json(f"locks/{self.name}.json") or {}
        if current.get("owner") == self.owner:
            current.update({"status": "released", "released_at": int(time.time())})
            self.store.write_json(f"locks/{self.name}.json", current, purpose="lock")
        self.released = True

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *_: object) -> None:
        self.release()


class StateStore:
    def __init__(self, b2: B2Transport, config: BoinkConfig):
        self.b2 = b2
        self.config = config

    def key(self, suffix: str) -> str:
        return self.config.state_key(suffix)

    def read_json(self, suffix: str) -> dict[str, Any] | list[Any] | None:
        return self.b2.read_json(self.key(suffix))

    def write_json(self, suffix: str, value: Any, *, purpose: str) -> None:
        self.b2.write_json(self.key(suffix), value, purpose=purpose)

    def acquire_lock(
        self, name: str, *, ttl_seconds: int, owner: str | None = None
    ) -> DurableLock:
        now = int(time.time())
        current = self.read_json(f"locks/{name}.json") or {}
        if current.get("status") == "held" and int(current.get("expires_at", 0)) > now:
            if owner and current.get("owner") == owner:
                return DurableLock(
                    self,
                    name,
                    owner,
                    int(current["expires_at"]),
                )
            raise RuntimeError(f"A live {name} coordinator lock already exists")
        owner = owner or uuid.uuid4().hex
        value = {
            "version": 1,
            "name": name,
            "owner": owner,
            "status": "held",
            "acquired_at": now,
            "expires_at": now + ttl_seconds,
        }
        self.write_json(f"locks/{name}.json", value, purpose="lock")
        observed = self.read_json(f"locks/{name}.json") or {}
        if observed.get("owner") != owner or observed.get("status") != "held":
            raise RuntimeError(f"Could not establish the {name} coordinator lock")
        return DurableLock(self, name, owner, now + ttl_seconds)

    def manifest_suffix(self, generation: str) -> str:
        return f"refresh/generations/{generation}/manifest.json"

    def state_suffix(self, generation: str) -> str:
        return f"refresh/generations/{generation}/state.json"

    def shard_suffix(self, generation: str, round_number: int, shard: int) -> str:
        return f"refresh/generations/{generation}/shards/round-{round_number}-shard-{shard}.json"

    def progress_suffix(self, generation: str, round_number: int, shard: int) -> str:
        return f"refresh/generations/{generation}/progress/round-{round_number}-shard-{shard}.json"

    def write_manifest(self, manifest: RefreshManifest) -> None:
        self.write_json(
            self.manifest_suffix(manifest.generation),
            manifest.to_dict(),
            purpose="refresh-manifest",
        )

    def read_manifest(self, generation: str) -> RefreshManifest:
        value = self.read_json(self.manifest_suffix(generation))
        if not isinstance(value, dict):
            raise TypeError("Refresh manifest is missing")
        return RefreshManifest.from_dict(value)

    def write_generation_state(self, generation: str, value: dict[str, Any]) -> None:
        body = {
            "version": 1,
            "generation": generation,
            **value,
            "updated_at": int(time.time()),
        }
        self.write_json(self.state_suffix(generation), body, purpose="refresh-state")

    def read_generation_state(self, generation: str) -> dict[str, Any] | None:
        value = self.read_json(self.state_suffix(generation))
        return value if isinstance(value, dict) else None

    def write_shards(self, plans: list[ShardPlan]) -> None:
        for plan in plans:
            self.write_json(
                self.shard_suffix(plan.generation, plan.round_number, plan.shard),
                plan.to_dict(),
                purpose="refresh-shard",
            )

    def read_shard(self, generation: str, round_number: int, shard: int) -> ShardPlan:
        value = self.read_json(self.shard_suffix(generation, round_number, shard))
        if not isinstance(value, dict):
            return ShardPlan(generation, round_number, shard, [])
        return ShardPlan.from_dict(value)

    def write_progress(
        self,
        generation: str,
        round_number: int,
        shard: int,
        *,
        completed: set[str],
        failed: dict[str, str],
        deadline_reached: bool,
    ) -> None:
        value = {
            "version": 1,
            "generation": generation,
            "round": round_number,
            "shard": shard,
            "completed": sorted(completed),
            "failed": dict(sorted(failed.items())),
            "deadline_reached": deadline_reached,
            "updated_at": int(time.time()),
        }
        self.write_json(
            self.progress_suffix(generation, round_number, shard),
            value,
            purpose="refresh-progress",
        )

    def read_completed(self, generation: str, through_round: int) -> set[str]:
        completed: set[str] = set()
        for round_number in range(through_round + 1):
            for shard in range(self.config.refresh_workers):
                value = self.read_json(
                    self.progress_suffix(generation, round_number, shard)
                )
                if isinstance(value, dict):
                    completed.update(str(key) for key in value.get("completed", []))
        return completed

    def current(self) -> dict[str, Any] | None:
        value = self.read_json("refresh/current.json")
        return value if isinstance(value, dict) else None

    def set_current(self, value: dict[str, Any]) -> None:
        self.write_json("refresh/current.json", value, purpose="active-generation")

    def cleanup_backlog(self) -> dict[str, Any]:
        value = self.read_json("refresh/cleanup-backlog.json")
        if not isinstance(value, dict):
            return {"version": 1, "keys": [], "updated_at": int(time.time())}
        keys = sorted({str(key) for key in value.get("keys", [])})
        return {**value, "version": 1, "keys": keys}

    def set_cleanup_backlog(
        self, keys: list[str], *, reason: str, not_before: int = 0
    ) -> None:
        self.write_json(
            "refresh/cleanup-backlog.json",
            {
                "version": 1,
                "keys": sorted(set(keys)),
                "reason": reason,
                "not_before": int(not_before),
                "updated_at": int(time.time()),
            },
            purpose="cleanup-backlog",
        )

    def known_generation_states(self) -> list[dict[str, Any]]:
        records = self.b2.list_objects(self.key("refresh/generations/"))
        states: list[dict[str, Any]] = []
        for record in records:
            if not record.key.endswith("/state.json"):
                continue
            value = self.b2.read_json(record.key)
            if isinstance(value, dict):
                states.append(value)
        return states


def key_identity(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()
