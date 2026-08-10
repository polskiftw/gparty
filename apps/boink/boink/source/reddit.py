from __future__ import annotations

import base64
import binascii
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Any

SUBREDDIT_NAME_PATTERN = re.compile(r"^[A-Za-z0-9_]{3,21}$")


def normalize_source(value: str) -> str:
    candidate = str(value or "").strip()
    if not candidate:
        return ""
    if candidate.startswith(("https://", "http://")):
        match = re.fullmatch(
            r"https?://(?:www\.|old\.|new\.)?reddit\.com/r/([A-Za-z0-9_]{3,21})(?:/.*)?",
            candidate,
            flags=re.IGNORECASE,
        )
        if not match:
            return ""
        candidate = match.group(1)
    else:
        candidate = candidate.strip("/")
        if candidate.lower().startswith("r/"):
            candidate = candidate[2:].split("/", 1)[0]
    if not SUBREDDIT_NAME_PATTERN.fullmatch(candidate):
        return ""
    return f"https://www.reddit.com/r/{candidate}/new/"


def normalize_sources(values: list[Any]) -> list[str]:
    normalized_sources: list[str] = []
    seen: set[str] = set()
    for value in values:
        normalized = normalize_source(str(value))
        identity = normalized.lower()
        if not normalized or identity in seen:
            continue
        seen.add(identity)
        normalized_sources.append(normalized)
    if not normalized_sources:
        raise RuntimeError(
            "No valid Reddit sources are configured in _internal/reddit-sources.json"
        )
    return normalized_sources


def source_identity(source: str) -> str:
    return hashlib.sha256(source.lower().encode("utf-8")).hexdigest()[:16]


def write_cookies(path: Path) -> None:
    encoded = os.getenv("REDDIT_COOKIES_BASE64", "").strip()
    if not encoded:
        raise RuntimeError("Missing REDDIT_COOKIES_BASE64")
    try:
        value = base64.b64decode(encoded, validate=True).decode(
            "utf-8-sig", errors="strict"
        )
    except (binascii.Error, UnicodeDecodeError, ValueError) as exc:
        raise RuntimeError("REDDIT_COOKIES_BASE64 is not valid Base64 text") from exc
    if "reddit.com" not in value.lower() or not value.lstrip().startswith(
        ("# Netscape HTTP Cookie File", "# HTTP Cookie File")
    ):
        raise RuntimeError("Decoded Reddit cookies are not a Netscape cookies.txt file")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")
    try:
        path.chmod(0o600)
    except OSError:
        pass


def build_gallery_dl_config(
    settings: dict[str, Any],
    *,
    archive_path: Path,
    cookies_path: Path,
    config_path: Path,
    partial_root: Path,
) -> None:
    write_cookies(cookies_path)
    user_agent = str(settings["browser_user_agent"]).strip()
    headers = {"User-Agent": user_agent}
    value = {
        "extractor": {
            "user-agent": user_agent,
            "filename": "{filename[b:120]}.{extension}",
            "archive": str(archive_path),
            "retries": int(settings.get("download_retries", 4)),
            "timeout": int(settings.get("download_timeout_seconds", 45)),
            "sleep-request": (
                f"{float(settings.get('reddit_request_delay_min_seconds', 2)):g}-"
                f"{float(settings.get('reddit_request_delay_max_seconds', 4)):g}"
            ),
            "sleep-429": float(settings.get("reddit_429_backoff_seconds", 60)),
            "cookies": str(cookies_path),
            "cookies-update": str(cookies_path),
            "reddit": {"api": "rest", "cookies": str(cookies_path), "videos": True},
            "ytdl": {"enabled": True, "raw-options": {"http_headers": headers}},
        },
        "downloader": {
            "part-directory": str(partial_root),
            "ytdl": {
                "format": "bestvideo*+bestaudio/best",
                "forward-cookies": True,
                "raw-options": {"http_headers": headers},
            },
        },
    }
    config_path.parent.mkdir(parents=True, exist_ok=True)
    partial_root.mkdir(parents=True, exist_ok=True)
    config_path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def run_gallery_dl(
    source: str,
    destination: Path,
    settings: dict[str, Any],
    config_path: Path,
) -> int:
    destination.mkdir(parents=True, exist_ok=True)
    command = [
        "gallery-dl",
        "--quiet",
        "--config",
        str(config_path),
        "-o",
        f"extractor.base-directory={destination}",
        "--post-range",
        f"1-{int(settings.get('posts_per_subreddit_per_scan', 50))}",
        "--abort",
        str(int(settings.get("stop_after_consecutive_archived_posts", 15))),
        source,
    ]
    result = subprocess.run(
        command,
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return int(result.returncode)
