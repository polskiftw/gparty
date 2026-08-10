# GParty

GParty is a private media system built around **Boink**, Backblaze B2, Cloudflare R2, and a Cloudflare Worker viewer.

## Current architecture

```text
Reddit
  |
  v
Boink acquisition
  |
  +--> runner-local staging
  |
  +--> canonical Backblaze B2 library
            |
            v
      Boink refresh
            |
            +--> bounded daily R2 generation
            +--> gallery-index.json
                         |
                         v
                 Cloudflare Worker
```

## Reddit source configuration

There is one authoritative source list:

```text
_internal/reddit-sources.json
```

It lives in Cloudflare R2. Boink reads that object during acquisition preparation, validates and normalizes its entries, and fails closed if it is missing, empty, malformed, or contains no valid subreddit sources.

The repository does not contain fallback subreddit names. GitHub Actions does not supply numbered subreddit-source secrets. The certificate-protected viewer can add sources through its source-management API, which writes to the same R2 object.

## Boink durable state

Boink keeps its private durable state under:

```text
_internal/boink/
```

The acquisition history database lives at:

```text
_internal/boink/acquire/gallery-dl-archive.sqlite3
```

The canonical media library lives in B2 under:

```text
gallery/
```

## R2 publication

Boink refresh builds byte-bounded generations under:

```text
gallery/generations/
```

Only a fully verified generation is published through:

```text
gallery-index.json
```

Refresh uses bounded workers and replacement rounds for unfinished objects. Cleanup is idempotent and can be retried independently.

## GitHub Actions secrets

Boink acquisition and refresh use the storage and routing credentials they require, including B2, R2, Tailscale, and Reddit cookies. Reddit subreddit names themselves are not stored as GitHub Actions secrets.

## Repository layout

| Path | Purpose |
|---|---|
| `boink/` | Acquisition, B2 storage, R2 refresh, durable state, and source handling |
| `boink.json` | Boink operational configuration |
| `settings.json` | Downloader limits, delays, formats, and browser identity |
| `.github/workflows/boink.yml` | Boink acquisition, refresh, recovery, finalize, and cleanup workflow |
| `worker/` | Certificate-protected viewer and source-management API |
| `tests/` | Focused Boink behavior tests |

## Production guard

Scheduled Boink jobs are gated by the repository variable:

```text
BOINK_PRODUCTION_ENABLED
```

Until that variable is deliberately enabled, the scheduled definitions remain inert.

## License

See `LICENSE`.
