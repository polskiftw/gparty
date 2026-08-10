from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]


def required_env(name: str) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        raise RuntimeError(f"Missing required environment variable: {name}")
    return value


@dataclass(frozen=True)
class BoinkConfig:
    b2_bucket_name: str
    canonical_prefix: str = "gallery/"
    internal_prefix: str = "_internal/boink/"
    r2_generation_prefix: str = "gallery/generations/"
    r2_index_key: str = "gallery-index.json"
    reddit_sources_key: str = "_internal/reddit-sources.json"
    target_bytes: int = 6_000_000_000
    acquire_upload_workers: int = 8
    refresh_workers: int = 8
    recovery_rounds: int = 2
    max_attempts: int = 6
    job_budget_seconds: int = 19_800
    abandoned_after_seconds: int = 86_400
    publication_cleanup_grace_seconds: int = 90

    @classmethod
    def load(cls, path: Path | None = None) -> BoinkConfig:
        config_path = path or Path(
            os.getenv("BOINK_CONFIG", ROOT / "config" / "boink.json")
        )
        payload = json.loads(config_path.read_text(encoding="utf-8"))
        if not isinstance(payload, dict):
            raise TypeError("boink.json must contain a JSON object")
        if os.getenv("BOINK_B2_BUCKET_NAME", "").strip():
            payload["b2_bucket_name"] = os.environ["BOINK_B2_BUCKET_NAME"].strip()
        config = cls(**payload)
        config.validate()
        return config

    def validate(self) -> None:
        prefixes = (
            self.canonical_prefix,
            self.internal_prefix,
            self.r2_generation_prefix,
        )
        if any(
            not value or not value.endswith("/") or value.startswith("/")
            for value in prefixes
        ):
            raise RuntimeError(
                "Boink prefixes must be non-empty relative keys ending in '/'"
            )
        if self.internal_prefix.startswith(self.canonical_prefix):
            raise RuntimeError(
                "Boink internal state may not be inside the canonical gallery prefix"
            )
        if not self.r2_generation_prefix.startswith(self.canonical_prefix):
            raise RuntimeError(
                "R2 generations must remain under gallery/ for Worker compatibility"
            )
        if self.r2_index_key.startswith(self.canonical_prefix):
            raise RuntimeError("The active index may not be part of gallery selection")
        positive = {
            "target_bytes": self.target_bytes,
            "acquire_upload_workers": self.acquire_upload_workers,
            "refresh_workers": self.refresh_workers,
            "max_attempts": self.max_attempts,
            "job_budget_seconds": self.job_budget_seconds,
        }
        for name, value in positive.items():
            if not isinstance(value, int) or value < 1:
                raise RuntimeError(f"{name} must be a positive integer")
        if self.recovery_rounds < 0:
            raise RuntimeError("recovery_rounds cannot be negative")
        if self.publication_cleanup_grace_seconds < 0:
            raise RuntimeError("publication_cleanup_grace_seconds cannot be negative")

    def state_key(self, suffix: str) -> str:
        suffix = suffix.lstrip("/")
        if not suffix or ".." in Path(suffix).parts:
            raise ValueError("Unsafe state suffix")
        return f"{self.internal_prefix}{suffix}"


def load_collector_settings(path: Path | None = None) -> dict[str, Any]:
    settings_path = path or Path(
        os.getenv("SETTINGS_PATH", ROOT / "config" / "settings.json")
    )
    payload = json.loads(settings_path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise TypeError("settings.json must contain a JSON object")
    allowed = payload.get("allowed_extensions")
    if not isinstance(allowed, list) or not allowed:
        raise RuntimeError("settings.json must contain allowed_extensions")
    user_agent = str(payload.get("browser_user_agent", "")).strip()
    if not user_agent or "\n" in user_agent or "\r" in user_agent:
        raise RuntimeError(
            "settings.json browser_user_agent must be one non-empty line"
        )
    numeric = {
        "posts_per_subreddit_per_scan": int(
            payload.get("posts_per_subreddit_per_scan", 50)
        ),
        "stop_after_consecutive_archived_posts": int(
            payload.get("stop_after_consecutive_archived_posts", 15)
        ),
        "download_retries": int(payload.get("download_retries", 4)),
        "download_timeout_seconds": int(payload.get("download_timeout_seconds", 45)),
        "maximum_file_size_mb": int(payload.get("maximum_file_size_mb", 500)),
    }
    if any(value < 1 for value in numeric.values()):
        raise RuntimeError(
            "Collector counts, limits, retries, and timeouts must be positive"
        )
    delay_min = float(payload.get("reddit_request_delay_min_seconds", 2))
    delay_max = float(payload.get("reddit_request_delay_max_seconds", 4))
    backoff = float(payload.get("reddit_429_backoff_seconds", 60))
    if delay_min < 0 or delay_max < delay_min or backoff < 0:
        raise RuntimeError("Collector delay values are invalid")
    return payload
