# GParty — DGD Guide

This document describes the current production design only.

# 1. Boink

Boink owns two jobs:

1. Acquire new Reddit media into the canonical Backblaze B2 library.
2. Build and publish bounded Cloudflare R2 generations for the viewer.

# 2. Reddit sources

The only authoritative subreddit list is:

```text
_internal/reddit-sources.json
```

That object lives in R2.

Boink does not read subreddit names from repository settings, numbered GitHub secrets, environment-variable source slots, or built-in fallback values.

During acquisition preparation Boink:

1. Reads `_internal/reddit-sources.json` from R2.
2. Requires a JSON list or an object containing a `sources` list.
3. Normalizes subreddit names and Reddit subreddit URLs.
4. Removes case-insensitive duplicates.
5. Fails closed when the object is missing, empty, malformed, or has no valid sources.
6. Writes a private runtime snapshot into Boink durable state without logging the source names.

The certificate-protected viewer can add a subreddit through `POST /api/sources`. That endpoint writes to the same R2 object, so there is one configuration surface.

# 3. Acquisition

Acquisition runs in three phases:

```text
prepare -> download -> commit
```

`prepare` reads the R2 source list and restores Boink's acquisition-history database from B2.

`download` routes source traffic through the configured Tailscale exit node and stages completed files on the GitHub runner.

`commit` first verifies that direct GitHub routing has been restored, then uploads verified staged files to canonical B2 storage with bounded workers.

The current acquisition-history object is:

```text
_internal/boink/acquire/gallery-dl-archive.sqlite3
```

If that history object is absent, acquisition fails closed rather than starting a fresh mass download.

# 4. Canonical B2 library

Canonical media objects live under:

```text
gallery/
```

Boink writes acquisition provenance metadata and verifies uploaded size and SHA-1 through the B2 transport layer.

# 5. R2 refresh

The daily refresh path is:

```text
prepare
-> round 0 workers
-> recovery 1
-> replacement round 1
-> recovery 2
-> replacement round 2
-> finalize
```

Refresh selects a bounded amount of canonical B2 media, builds an R2 generation under:

```text
gallery/generations/
```

and publishes only after full verification.

The active viewer index is:

```text
gallery-index.json
```

Incomplete generations are never published as active.

# 6. Cleanup

Cleanup retries obsolete-generation and abandoned-generation deletion idempotently. Publication and cleanup are deliberately separated so cleanup failure cannot invalidate an already verified publication.

# 7. Workflow controls

The workflow is:

```text
.github/workflows/boink.yml
```

Manual modes are:

```text
acquire
refresh
cleanup
```

Scheduled jobs are gated by:

```text
BOINK_PRODUCTION_ENABLED
```

# 8. Required private credentials

Boink uses private credentials for:

- Backblaze B2 access
- Cloudflare R2 access
- Tailscale routing
- Reddit browser cookies

Subreddit names are not GitHub Actions secrets. They live only in the R2 source object and Boink's private runtime/durable snapshots.

# 9. Viewer source manager

The certificate-protected viewer exposes an add-source control. A successful addition updates `_internal/reddit-sources.json` and the next Boink acquisition reads the updated list.

# 10. Failure posture

The intended rule is fail closed rather than silently substituting another configuration source. Missing or malformed source configuration, missing acquisition history, unverified routing, or failed storage verification stops the affected operation before unsafe continuation.
