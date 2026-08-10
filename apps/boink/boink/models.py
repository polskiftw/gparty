from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass, field
from pathlib import PurePosixPath
from typing import Any


def _safe_key(key: str) -> str:
    value = str(key)
    parts = PurePosixPath(value).parts
    if not value or value.startswith("/") or ".." in parts or "\\" in value:
        raise ValueError(f"Unsafe object key: {value!r}")
    return value


@dataclass(frozen=True)
class ObjectRecord:
    key: str
    size: int
    sha1: str
    file_id: str = ""
    content_type: str = "application/octet-stream"
    metadata: dict[str, str] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "key", _safe_key(self.key))
        if self.size <= 0:
            raise ValueError("Object size must be positive")
        if (
            self.sha1
            and self.sha1 != "none"
            and (
                len(self.sha1) != 40
                or any(ch not in "0123456789abcdef" for ch in self.sha1.lower())
            )
        ):
            raise ValueError("Object SHA-1 must be a 40-character hexadecimal digest")

    @property
    def extension(self) -> str:
        name = PurePosixPath(self.key).name
        return name.rsplit(".", 1)[1].lower() if "." in name else ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> ObjectRecord:
        return cls(
            key=str(value["key"]),
            size=int(value["size"]),
            sha1=str(value.get("sha1", "")),
            file_id=str(value.get("file_id", "")),
            content_type=str(value.get("content_type", "application/octet-stream")),
            metadata={
                str(k): str(v) for k, v in dict(value.get("metadata", {})).items()
            },
        )


@dataclass(frozen=True)
class ManifestEntry:
    source_key: str
    destination_key: str
    size: int
    sha1: str
    file_id: str
    extension: str
    content_type: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "source_key", _safe_key(self.source_key))
        object.__setattr__(self, "destination_key", _safe_key(self.destination_key))
        if self.size <= 0:
            raise ValueError("Manifest entry size must be positive")
        if not self.destination_key.startswith("gallery/"):
            raise ValueError(
                "Worker-compatible destination keys must start with gallery/"
            )

    @property
    def identity(self) -> str:
        return hashlib.sha256(self.source_key.encode("utf-8")).hexdigest()

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> ManifestEntry:
        return cls(
            source_key=str(value["source_key"]),
            destination_key=str(value["destination_key"]),
            size=int(value["size"]),
            sha1=str(value["sha1"]),
            file_id=str(value.get("file_id", "")),
            extension=str(value.get("extension", "")).lower(),
            content_type=str(value.get("content_type", "application/octet-stream")),
        )


@dataclass
class RefreshManifest:
    generation: str
    created_at: int
    seed: str
    target_bytes: int
    inventory_objects: int
    inventory_bytes: int
    entries: list[ManifestEntry]
    version: int = 1
    selection_algorithm: str = "seeded-shuffle-under-budget-v1"

    @property
    def selected_bytes(self) -> int:
        return sum(item.size for item in self.entries)

    def validate(self) -> None:
        if self.version != 1 or not self.generation:
            raise ValueError("Unsupported or invalid refresh manifest")
        if self.target_bytes < 1 or self.inventory_objects < len(self.entries):
            raise ValueError("Invalid manifest accounting")
        source_keys = [item.source_key for item in self.entries]
        destination_keys = [item.destination_key for item in self.entries]
        if len(source_keys) != len(set(source_keys)) or len(destination_keys) != len(
            set(destination_keys)
        ):
            raise ValueError("Manifest contains duplicate keys")
        wanted_prefix = f"gallery/generations/{self.generation}/"
        if any(
            not item.destination_key.startswith(wanted_prefix) for item in self.entries
        ):
            raise ValueError("Manifest destination belongs to another generation")

    def to_dict(self) -> dict[str, Any]:
        self.validate()
        return {
            "version": self.version,
            "generation": self.generation,
            "created_at": self.created_at,
            "seed": self.seed,
            "selection_algorithm": self.selection_algorithm,
            "target_bytes": self.target_bytes,
            "inventory_objects": self.inventory_objects,
            "inventory_bytes": self.inventory_bytes,
            "selected_objects": len(self.entries),
            "selected_bytes": self.selected_bytes,
            "entries": [item.to_dict() for item in self.entries],
        }

    def to_json(self) -> bytes:
        return (json.dumps(self.to_dict(), indent=2, sort_keys=True) + "\n").encode(
            "utf-8"
        )

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> RefreshManifest:
        manifest = cls(
            version=int(value.get("version", 0)),
            generation=str(value["generation"]),
            created_at=int(value["created_at"]),
            seed=str(value["seed"]),
            selection_algorithm=str(value.get("selection_algorithm", "")),
            target_bytes=int(value["target_bytes"]),
            inventory_objects=int(value["inventory_objects"]),
            inventory_bytes=int(value["inventory_bytes"]),
            entries=[
                ManifestEntry.from_dict(item) for item in value.get("entries", [])
            ],
        )
        if int(value.get("selected_objects", len(manifest.entries))) != len(
            manifest.entries
        ):
            raise ValueError("Manifest selected-object accounting mismatch")
        if (
            int(value.get("selected_bytes", manifest.selected_bytes))
            != manifest.selected_bytes
        ):
            raise ValueError("Manifest selected-byte accounting mismatch")
        manifest.validate()
        return manifest

    @classmethod
    def from_json(cls, payload: bytes | str) -> RefreshManifest:
        return cls.from_dict(json.loads(payload))


@dataclass(frozen=True)
class ShardPlan:
    generation: str
    round_number: int
    shard: int
    entries: list[ManifestEntry]

    @property
    def bytes(self) -> int:
        return sum(item.size for item in self.entries)

    def to_dict(self) -> dict[str, Any]:
        return {
            "version": 1,
            "generation": self.generation,
            "round": self.round_number,
            "shard": self.shard,
            "objects": len(self.entries),
            "bytes": self.bytes,
            "entries": [item.to_dict() for item in self.entries],
        }

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> ShardPlan:
        plan = cls(
            generation=str(value["generation"]),
            round_number=int(value["round"]),
            shard=int(value["shard"]),
            entries=[
                ManifestEntry.from_dict(item) for item in value.get("entries", [])
            ],
        )
        if int(value.get("objects", len(plan.entries))) != len(plan.entries):
            raise ValueError("Shard object accounting mismatch")
        if int(value.get("bytes", plan.bytes)) != plan.bytes:
            raise ValueError("Shard byte accounting mismatch")
        return plan
