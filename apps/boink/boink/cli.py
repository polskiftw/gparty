from __future__ import annotations

import argparse
import json
import logging
import os
from pathlib import Path
from typing import Any

from . import __version__
from .acquire import commit_acquisition, download_acquisition, prepare_acquisition
from .config import BoinkConfig
from .refresh import (
    cleanup_abandoned_generations,
    finalize_refresh,
    plan_recovery_round,
    prepare_refresh,
    retry_cleanup_backlog,
    run_refresh_worker,
)
from .state import StateStore
from .storage.b2 import B2Transport
from .storage.r2 import R2Transport

log = logging.getLogger("boink")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="boink", description="GParty asset logistics")
    parser.add_argument("--version", action="version", version=f"Boink {__version__}")
    parser.add_argument("--config", type=Path)
    sub = parser.add_subparsers(dest="command", required=True)

    acquire = sub.add_parser("acquire", help="Acquire source media into canonical B2")
    acquire.add_argument("phase", choices=("prepare", "download", "commit"))
    acquire.add_argument("--settings", type=Path)

    refresh = sub.add_parser("refresh", help="Build and publish one R2 generation")
    refresh_sub = refresh.add_subparsers(dest="refresh_command", required=True)
    prepare = refresh_sub.add_parser("prepare")
    prepare.add_argument("--target-bytes", type=int)
    prepare.add_argument("--seed")
    worker = refresh_sub.add_parser("worker")
    worker.add_argument("--generation", required=True)
    worker.add_argument("--round", type=int, required=True)
    worker.add_argument("--shard", type=int, required=True)
    worker.add_argument("--budget-seconds", type=int)
    recover = refresh_sub.add_parser("recover")
    recover.add_argument("--generation", required=True)
    recover.add_argument("--round", type=int, required=True)
    finalize = refresh_sub.add_parser("finalize")
    finalize.add_argument("--generation", required=True)
    refresh_sub.add_parser("cleanup")

    parser.add_argument("--github-output", type=Path)
    return parser


def _write_outputs(path: Path | None, value: dict[str, Any]) -> None:
    if path is None:
        configured = os.getenv("GITHUB_OUTPUT", "").strip()
        path = Path(configured) if configured else None
    if path is None:
        return
    with path.open("a", encoding="utf-8") as handle:
        for name in (
            "generation",
            "published",
            "remaining_objects",
            "selected_objects",
            "selected_bytes",
        ):
            if name in value:
                scalar = value[name]
                if isinstance(scalar, bool):
                    scalar = str(scalar).lower()
                handle.write(f"{name}={scalar}\n")
        if "worker_matrix" in value:
            handle.write(
                "worker_matrix="
                + json.dumps(value["worker_matrix"], separators=(",", ":"))
                + "\n"
            )


def _write_summary(title: str, value: dict[str, Any]) -> None:
    path = os.getenv("GITHUB_STEP_SUMMARY", "").strip()
    if not path:
        return
    with Path(path).open("a", encoding="utf-8") as handle:
        handle.write(f"## {title}\n\n")
        for name, item in value.items():
            if isinstance(item, (dict, list)):
                continue
            label = name.replace("_", " ").capitalize()
            handle.write(f"- {label}: **{item}**\n")


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(
        level=os.getenv("LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s | %(levelname)s | %(name)s | %(message)s",
    )
    args = _parser().parse_args(argv)
    config = BoinkConfig.load(args.config)
    result: dict[str, Any]
    try:
        if args.command == "acquire":
            if args.phase == "prepare":
                result = prepare_acquisition(
                    config,
                    B2Transport.from_env(config),
                    R2Transport.from_env(config),
                    settings_path=args.settings,
                )
            elif args.phase == "download":
                result = download_acquisition(config)
            else:
                result = commit_acquisition(config, B2Transport.from_env(config))
            title = f"Boink acquisition {args.phase}"
        else:
            if args.refresh_command == "prepare":
                result = prepare_refresh(
                    config,
                    B2Transport.from_env(config),
                    R2Transport.from_env(config),
                    target_bytes=args.target_bytes,
                    seed=args.seed,
                )
            elif args.refresh_command == "worker":
                result = run_refresh_worker(
                    config,
                    B2Transport.from_env(config),
                    R2Transport.from_env(config),
                    generation=args.generation,
                    round_number=args.round,
                    shard=args.shard,
                    budget_seconds=args.budget_seconds,
                )
            elif args.refresh_command == "recover":
                result = plan_recovery_round(
                    config,
                    B2Transport.from_env(config),
                    R2Transport.from_env(config),
                    generation=args.generation,
                    round_number=args.round,
                )
            elif args.refresh_command == "finalize":
                result = finalize_refresh(
                    config,
                    B2Transport.from_env(config),
                    R2Transport.from_env(config),
                    generation=args.generation,
                )
            else:
                b2 = B2Transport.from_env(config)
                r2 = R2Transport.from_env(config)
                store = StateStore(b2, config)
                result = {
                    "backlog": retry_cleanup_backlog(store, r2, config),
                    "abandoned": cleanup_abandoned_generations(store, r2, config),
                }
            title = f"Boink refresh {args.refresh_command}"
    except Exception:
        log.exception("Boink operation failed safely")
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    _write_outputs(args.github_output, result)
    _write_summary(title, result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
