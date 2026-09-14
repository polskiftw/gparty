# GParty

GParty is a Cloudflare-hosted random media gallery with a storage pipeline that keeps a canonical media library in Backblaze B2 and publishes bounded, verified generations to Cloudflare R2 for delivery through a Cloudflare Worker.

The repository contains five runtime components:

- **Boink** (`apps/boink`) acquires media, maintains canonical B2 state, and builds publishable R2 generations.
- **gdupe** (`apps/gdupe`) is the native Windows duplicate manager for the canonical B2 library.
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
                                      v
                              Boink refresh selection
                                      |
                             verified R2 generation
                                      |
                               gallery-index.json
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

The GitHub Actions acquisition job verifies a private Tailscale exit route before source access, then restores and verifies the runner's direct route before allowing B2 writes. If a source or object fails, acquisition history is rolled back far enough for the affected source to be retried safely on a later run.

### R2 refresh and publication

A refresh builds a new immutable R2 generation from the canonical B2 inventory:

1. Inventory the canonical `gallery/` objects in B2.
2. Select a randomized set up to the configured byte budget.
3. Persist a durable manifest and split it into byte-balanced worker shards.
4. Download each selected B2 object to temporary runner storage and upload it to the generation namespace in R2.
5. Requeue only unfinished or unverifiable objects through the configured bounded recovery rounds.
6. Refuse publication unless every manifest object verifies in R2.
7. Publish the gallery index and remapped tag index, verify both bodies, and update the durable active-generation pointer.
8. Delete obsolete generation objects after the publication grace period, with failed cleanup retained as a retry backlog.

Publication uses a durable journal. If index publication fails, the Worker-facing gallery and tag indexes are restored to their previous known-good state before the refresh is marked failed.

### Fingerprinting

`gfingerd` continuously fills missing fingerprints directly into gdupe's existing local SQLite database, normally `%LOCALAPPDATA%\gdupe\gdupe.sqlite3`. It does not create a second fingerprint database, migrate existing fingerprints, or replace the gdupe data model. The existing database must already exist before gfingerd starts.

The daemon uses the same current gdupe fingerprint algorithms and native decoder implementation unchanged. It keeps persistent read-only B2 connections, uses a bounded download/prefetch/fingerprint pipeline, verifies the exact B2 file identity again before settlement, and writes through the existing key-plus-file-ID guarded database path. Static image and GIF work can proceed at cold boot while NVDEC-dependent video remains pending until the NVIDIA driver is ready.

Known malformed GIFs that hit the strict WIC geometry rejection are temporarily preserved locally and deferred by exact B2 file ID in gfingerd-specific operational state appended to the same SQLite database. See [`apps/gfingerd/README.md`](apps/gfingerd/README.md) and [`apps/gfingerd/TODO.md`](apps/gfingerd/TODO.md).

### Duplicate management

gdupe synchronizes a durable local SQLite inventory against stable snapshots of the live B2 `gallery/` listing. Unchanged objects reuse their SHA-256, static-image, crop-aware, animated-GIF, video, and timeline fingerprints. Byte-identical copies are removed automatically at startup; perceptual candidates are consolidated and shown in a minimal side-by-side review interface with the recommended survivor on the left.

All manual delete, exclude, and Process All actions are immediate. Destructive batches use exact B2 file-ID checks and a recovery journal, then regenerate and verify the canonical B2 inventory index before local settlement. The full behavior, configuration, and Windows build are documented in [`apps/gdupe/README.md`](apps/gdupe/README.md).

A later cleanup will remove gdupe's now-redundant foreground fingerprint-generation work after gfingerd has been proven against the live library. That behavior change is intentionally separate from the gfingerd introduction.

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

The source-management endpoint accepts subreddit names, `r/<name>` forms, and Reddit subreddit URLs. Writes require both a successfully verified Cloudflare client certificate and the expected request header, and updates use conditional R2 writes to avoid silently overwriting concurrent changes.

The browser UI supports random navigation, still/clip filtering, desktop tag filtering, responsive media sizing, and an add-source dialog. Random requests use bounded retries for transient failures.

## Email Worker

`apps/email-worker` is an independent Cloudflare Email Worker for `gooning.party`.

It accepts the fixed local parts `abuse` and `dmca`, plus aliases that end in a configured secret suffix. Accepted messages are parsed with `postal-mime`, the private suffix is removed from visible recipient content, MIME bodies and attachments are rebuilt, and the sanitized message is forwarded to the configured destination. Messages larger than 25 MiB are rejected.

Required bindings and secrets are defined in `apps/email-worker/wrangler.jsonc`:

- `EMAIL` — Cloudflare send-email binding
- `EMAIL_SECRET_SUFFIX`
- `FORWARD_TO`
- `OUTBOUND_FROM`

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

The active gallery index contains only `gallery/` keys. Boink keeps its internal state outside the canonical gallery prefix so internal objects cannot enter media selection.

## Configuration

### Boink

Primary configuration lives in:

- `apps/boink/config/boink.json` — storage prefixes, byte target, worker counts, retry/recovery limits, job budget, and cleanup timing.
- `apps/boink/config/settings.json` — source acquisition limits, request pacing, allowed extensions, maximum file size, download retries, and timeouts.

Boink reads B2 and R2 credentials from environment variables. The GitHub Actions workflow uses:

- `B2_KEY_ID`
- `B2_APPLICATION_KEY`
- `R2_ACCOUNT_ID`
- `R2_ACCESS_KEY_ID`
- `R2_SECRET_ACCESS_KEY`
- `R2_BUCKET_NAME`

Acquisition through GitHub Actions additionally uses `REDDIT_COOKIES_BASE64`, `TS_OAUTH_CLIENT_ID`, `TS_OAUTH_SECRET`, and `TS_EXIT_NODE_IP`.

Optional runtime overrides include `BOINK_CONFIG`, `BOINK_B2_BUCKET_NAME`, `BOINK_DATA_DIR`, and `SETTINGS_PATH`.

### Web Worker

The Worker requires:

- an R2 binding named `MEDIA_BUCKET`
- a `CONTACT_EMAIL` secret

`apps/web/wrangler.jsonc` contains a placeholder R2 bucket name for direct Wrangler use. The repository's deployment workflow generates its own temporary Wrangler configuration from repository secrets instead.

### gfingerd

`gfingerd` is normally configured through its native Win32 control panel. The user configuration lives at `%LOCALAPPDATA%\GParty\gfingerd.json`; the boot worker receives an ACL-restricted machine configuration and machine-scoped protected B2 credentials under `%ProgramData%\GParty`. Full setup and runtime details are in [`apps/gfingerd/README.md`](apps/gfingerd/README.md).

## GitHub Actions

### `boink`

`.github/workflows/boink.yml` supports manual `acquire`, `refresh`, and `cleanup` runs.

Scheduled definitions are also present:

- acquisition at minutes `3`, `18`, `33`, and `48` of each hour
- refresh daily at `08:17 UTC`

Scheduled jobs run only when the repository variable `BOINK_PRODUCTION_ENABLED` is set to `true`. Refresh and cleanup share an index-writer concurrency lock so only one publication path can modify the active gallery at a time.

### `update-cf-web`

`.github/workflows/update-cf-web.yml` is a manual deployment path for the web Worker. It checks out current `main`, validates expected Worker source invariants, builds a temporary Wrangler configuration, and deploys using Cloudflare credentials stored as repository secrets.

### `gdupe-build`

`.github/workflows/gdupe-build.yml` builds the native Windows application with its pinned vcpkg dependency graph, runs the core safety and matching tests, deploys the Qt runtime, and publishes a ready-to-run ZIP artifact.

### `gfingerd-build`

`.github/workflows/gfingerd-build.yml` independently builds, tests, audits, and packages the standalone Windows gfingerd artifact. It consumes the current gdupe database/decoder/fingerprint sources without altering gdupe's project or package.

## Development

### C++ / gdupe

gdupe targets C++23 and 64-bit Windows. Reproducible vcpkg/CMake build instructions and the credential capabilities required at runtime are in [`apps/gdupe/README.md`](apps/gdupe/README.md).

### C++ / gfingerd

`gfingerd` is a separate C++23, 64-bit Windows project. Reproducible CMake/vcpkg instructions and the boot-daemon behavior are documented in [`apps/gfingerd/README.md`](apps/gfingerd/README.md).

### Python / Boink

Boink targets Python 3.12.

```bash
python -m pip install -r requirements.txt
```

Run the Boink tests from the repository root:

```bash
PYTHONPATH=apps/boink python -m unittest discover -s apps/boink/tests
```

The CLI is exposed through the package module:

```bash
PYTHONPATH=apps/boink python -m boink --help
```

Storage operations require the corresponding B2/R2 environment variables and existing durable state.

### Web Worker

```bash
cd apps/web
npm install
npm run dev
```

Provide a valid `MEDIA_BUCKET` binding and `CONTACT_EMAIL` secret for a functional local Worker environment.

The repository includes Python source-layout tests for the viewer:

```bash
python -m unittest discover -s apps/web/tests
```

### Email Worker

```bash
cd apps/email-worker
npm install
npm run check
npm run dev
```

Deployment is available through `npm run deploy` once the Cloudflare email binding and required secrets are configured.

## License

This project is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE).
