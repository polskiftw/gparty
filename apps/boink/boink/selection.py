from __future__ import annotations

import random
from collections.abc import Iterable

from .models import ManifestEntry, ObjectRecord, ShardPlan


def select_random_budget(
    inventory: Iterable[ObjectRecord],
    target_bytes: int,
    seed: str,
) -> list[ObjectRecord]:
    """Seeded random order, greedily packed without exceeding the byte target.

    If every candidate is larger than the budget, the smallest object is selected so
    a refresh remains useful and the exceptional overshoot is obvious in its manifest.
    """
    if target_bytes < 1:
        raise ValueError("target_bytes must be positive")
    unique: dict[str, ObjectRecord] = {}
    for item in inventory:
        if item.key.startswith("_internal/") or not item.key.startswith("gallery/"):
            continue
        if item.key in unique:
            raise ValueError(f"Duplicate inventory key: {item.key}")
        unique[item.key] = item
    candidates = list(unique.values())
    rng = random.Random(seed)
    rng.shuffle(candidates)
    selected: list[ObjectRecord] = []
    total = 0
    for item in candidates:
        if total + item.size <= target_bytes:
            selected.append(item)
            total += item.size
    if not selected and candidates:
        selected.append(min(candidates, key=lambda item: (item.size, item.key)))
    return selected


def byte_balanced_shards(
    entries: Iterable[ManifestEntry],
    worker_count: int,
    generation: str,
    round_number: int,
) -> list[ShardPlan]:
    """Longest-processing-time greedy balancing using exact object bytes."""
    if worker_count < 1:
        raise ValueError("worker_count must be positive")
    bins: list[list[ManifestEntry]] = [[] for _ in range(worker_count)]
    loads = [0] * worker_count
    for item in sorted(entries, key=lambda value: (-value.size, value.source_key)):
        index = min(range(worker_count), key=lambda value: (loads[value], value))
        bins[index].append(item)
        loads[index] += item.size
    return [
        ShardPlan(
            generation=generation, round_number=round_number, shard=index, entries=items
        )
        for index, items in enumerate(bins)
    ]


def unfinished_entries(
    entries: Iterable[ManifestEntry], completed_source_keys: set[str]
) -> list[ManifestEntry]:
    return [item for item in entries if item.source_key not in completed_source_keys]
