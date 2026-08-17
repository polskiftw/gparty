# GParty

GParty is a Cloudflare-hosted random media gallery with a storage pipeline that keeps a canonical media library in Backblaze B2 and publishes bounded, verified generations to Cloudflare R2 for delivery through a Cloudflare Worker.

The repository contains five runtime components:

- **Boink** (`apps/boink`) acquires media, maintains canonical B2 state, and builds publishable R2 generations.
- **gdupe** (`apps/gdupe`) is the native Windows duplicate review, preview, and deletion manager for the canonical B2 library.
- **gfingerd** (`apps/gfingerd`) is the standalone Windows daemon that keeps gdupe's existing fingerprint database populated from read-only B2 access.
- **Web** (`apps/web`) serves the gallery UI, random-selection API, tag filtering, media delivery, and managed-source endpoint.
- **Email Worker** (`apps/email-worker`) receives mail for `gooning.party`, sanitizes accepted recipient aliases, and forwards the resulting message through Cloudflare Email Routing.

## Architecture

```text
Reddit sources
     |
     v
GitHub Actions / Boink acquisition
     |
     +----> runner staging ----> canonical B2 gallery
                                      |
                         +------------+-------------+
                         |                          |
                         v                          v
                    gfingerd                   Boink refresh
                         |                          |
                         v                          v
               gdupe.sqlite3              verified R2 generation
                         |                          |
                         v                          v
                gdupe review UI             gallery-index.json
                                                    |
                                             Cloudflare Worker
                                                    |
                                                 browser

browser POST /api/sources ----> R2 managed source list ----> next acquisition
```

B2 is the durable canonical media store. R2 contains the currently published gallery generation and the indexes consumed by the web Worker.

## Media lifecycle

### Acquisition

Boink acquisition is split into three explicit phases:

1. `prepare` reads the managed Reddit source list from R2, restores the durable `gallery-dl` archive from B2, snapshots the source configuration, and creates a new run state.
2. `download` stages source media completely on the runner before any canonical storage writes occur.
3. `commit` validates staged files, generates stable gallery object keys, uploads eligible media to B2 with bounded concurrency, verifies uploaded size and SHA-1, and persists the updated acquisition history.

The GitHub Actions acquisition job verifies a private Tailscale exit route before source access, then restores and verifies the runner's direct route before allowing B2 writes. Failed source/object history is rolled back far enough for safe retry on a later run.

### R2 refresh and publication

A refresh builds a new immutable R2 generation from the canonical B2 inventory. It selects media up to the configured byte budget, persists a durable manifest, stages and verifies the generation, publishes the gallery/tag indexes only after every manifest object verifies, then removes obsolete generations after the publication grace period. Publication uses a durable journal so failed index publication can restore the prior known-good state.

### Fingerprinting

`gfingerd` starts at machine boot as Windows SYSTEM, before sign-in, and continuously fills fingerprints directly into gdupe's existing local SQLite database:

```text
%LOCALAPPDATA%\gdupe\gdupe.sqlite3
```

There is **no second fingerprint database, migration, adoption pass, or copied fingerprint registry**. Existing gdupe fingerprints are already finished work. Missing or invalidated `objects` rows become gfingerd's backlog.

The daemon deliberately compiles the same native decoder and fingerprint implementation gdupe currently uses, unchanged: fingerprint version 3, 48 video sample frames, and 32 GIF sample frames. It uses libjpeg-turbo, WIC PNG/GIF, statically linked libwebp, native MP4/WebM demux, and NVIDIA-driver NVDEC. No FFmpeg, Media Foundation, Qt, FLTK, Electron, or .NET UI dependency is introduced.

Each result is settled through gdupe's existing database writer, which requires both the object key and exact B2 file ID to still match. gfingerd also rechecks the live B2 identity before settlement, so Boink may add or replace media while fingerprinting runs without allowing a stale result onto a replacement object.

The same SQLite file contains only three gfingerd-specific operational tables: `gfingerd_failures`, `gfingerd_deferred_gifs`, and `gfingerd_metadata`. They store daemon retry/deferred/scan state and do not duplicate gdupe's inventory or fingerprint columns.

At cold boot, JPEG/PNG/WebP/GIF work can proceed before the NVIDIA driver is ready. MP4/M4V/WebM work remains pending until NVDEC becomes available and does not consume its media-failure budget simply because the driver has not initialized yet.

The known malformed-GIF geometry rejection is temporarily deferred by exact B2 file ID. The original GIF and JSON recovery note are preserved under `%LOCALAPPDATA%\GParty\deferred-gifs\`; its exact-version deferral lives in `gfingerd_deferred_gifs` in the normal gdupe database. A replacement B2 file ID is immediately eligible again. See [`apps/gfingerd/TODO.md`](apps/gfingerd/TODO.md) for the proper normalization/reprocessing follow-up.

### Duplicate management

gdupe consumes the durable inventory and fingerprints for duplicate analysis, preview, review, survivor selection, exclusions, deletion, and recovery. A planned follow-up will remove gdupe's now-redundant foreground fingerprint generation after gfingerd has been proven against the live library; gfingerd will then be the only process that collects new fingerprints.

All manual delete, exclude, and Process All actions remain gdupe responsibilities. Destructive batches use exact B2 file-ID checks and a recovery journal, then regenerate and verify the canonical B2 inventory index before local settlement. Full behavior is documented in [`apps/gdupe/README.md`](apps/gdupe/README.md).

## Web Worker

`apps/web/src/worker.js` wraps the viewer with restrictive response headers, including a content security policy, permissions policy, `X-Content-Type-Options`, no-referrer policy, and crawler directives that disable indexing and archiving.

The viewer exposes these routes:

| Route | Purpose |
| --- | --- |
| `/` | Gallery UI |
| `/api/random` | Random media selection, optionally filtered by media type and tags |
| `/api/tags` | Current tag catalog |
| `/api/sources` | Adds a managed Reddit source; `POST` only |
| `/media/<key>` | Streams gallery objects from R2, including byte-range requests |
| `/robots.txt` | Disallows crawling |

Supported media extensions are `jpg`, `jpeg`, `png`, `gif`, `webp`, `mp4`, `m4v`, and `webm`. Tag selections use AND semantics: an item must contain every selected tag.

## Email Worker

`apps/email-worker` is an independent Cloudflare Email Worker for `gooning.party`. It accepts the fixed local parts `abuse` and `dmca`, plus aliases ending in a configured secret suffix; accepted mail is sanitized and forwarded through Cloudflare Email Routing.

## Storage layout

| Key or prefix | Store | Purpose |
| --- | --- | --- |
| `gallery/` | B2 | Canonical media library |
| `_internal/boink/` | B2 | Durable Boink manifests, progress, locks, history, and publication state |
| `_internal/gdupe/canonical-index-v1.json` | B2 | Verified canonical gdupe inventory index |
| `_internal/reddit-sources.json` | R2 | Managed acquisition source list |
| `gallery/generations/` | R2 | Published and staging media generations |
| `gallery-index.json` | R2 | Active Worker-facing media index |
| `_internal/tag-index-v1.json` | R2 | Worker-facing tag catalog and media/tag mapping |
| `%LOCALAPPDATA%\gdupe\gdupe.sqlite3` | Local SQLite | gdupe inventory/fingerprints plus namespaced gfingerd operational state |

The active gallery index contains only `gallery/` keys. Boink keeps internal state outside the canonical gallery prefix so internal objects cannot enter media selection.

## Configuration

Boink's primary configuration lives under `apps/boink/config`. It reads B2/R2 credentials from environment variables and the repository workflows provide production secrets. Web Worker bindings and Email Worker bindings/secrets are documented in their respective app directories.

`gfingerd` configuration is normally managed through its native Win32 GUI. User configuration lives at `%LOCALAPPDATA%\GParty\gfingerd.json`; boot startup installs a SYSTEM worker with machine-protected configuration/credentials. Full setup, status, build, and safety behavior are documented in [`apps/gfingerd/README.md`](apps/gfingerd/README.md).

## GitHub Actions

### `boink`

`.github/workflows/boink.yml` supports manual `acquire`, `refresh`, and `cleanup` runs and contains the production acquisition/refresh schedules.

### `update-cf-web`

`.github/workflows/update-cf-web.yml` is the manual deployment path for the Web Worker.

### `gdupe-build`

`.github/workflows/gdupe-build.yml` builds, tests, audits, and packages gdupe. **gfingerd does not modify or ride inside this build.**

### `gfingerd-build`

`.github/workflows/gfingerd-build.yml` independently builds, tests, audits, and packages the standalone `gfingerd.exe`. Its build consumes gdupe's existing database/decoder/fingerprint source files unchanged so output remains compatible without changing gdupe's project or package.

## Development

### C++ / gdupe

gdupe targets C++23 and 64-bit Windows. Reproducible vcpkg/CMake instructions are in [`apps/gdupe/README.md`](apps/gdupe/README.md).

### C++ / gfingerd

gfingerd is a separate C++23/64-bit Windows project under `apps/gfingerd`. Build and boot-daemon instructions are in [`apps/gfingerd/README.md`](apps/gfingerd/README.md).

### Python / Boink

Boink targets Python 3.12. Run its tests from the repository root with:

```bash
PYTHONPATH=apps/boink python -m unittest discover -s apps/boink/tests
```

### Web Worker

```bash
cd apps/web
npm install
npm run dev
```

### Email Worker

```bash
cd apps/email-worker
npm install
npm run check
npm run dev
```

## License

This project is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE).
