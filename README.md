# GParty

GParty is a self-contained media collector, Cloudflare R2 library, tag-aware random browser viewer, and local AI tagger.

The browser viewer validates random-item API responses and automatically retries transient, timed-out, empty, or malformed responses without replacing the currently displayed media.

## Architecture

```text
Reddit sources
      |
      v
GitHub Actions: Yoink
      |
      +--> gallery/<media objects>
      +--> gallery-index.json
      +--> _internal/gallery-dl-archive-v0.2.1.sqlite3
                 |
                 v
             Cloudflare R2
               /       \
              v         v
    Worker random viewer  Tag Time
              ^             |
              +-- private tag index
```

The shared storage contract is:

| Purpose | Value |
|---|---|
| Media prefix | `gallery/` |
| Gallery index | `gallery-index.json` |
| Private viewer-managed source list | `_internal/reddit-sources.json` |
| Private Tag Time index | `_internal/tag-index-v1.json` |
| Worker R2 binding | `MEDIA_BUCKET` |
| Bucket selector | `R2_BUCKET_NAME` |

## Repository layout

| Path | Purpose |
|---|---|
| `app.py` | Collector, private source-list merge, download history, R2 upload, and index generation |
| `settings.json` | Collector limits, delays, formats, and fallback sources |
| `worker/worker.js` | Security headers around the Worker routes |
| `worker/viewer.js` | Viewer HTML, random-media API, certificate-protected source API, and R2 media streaming |
| `worker/app.js` | Browser viewer and add-subreddit behavior |
| `worker/style.css` | Browser viewer styling |
| `worker/wrangler.jsonc` | Local Wrangler entrypoint, asset rules, limits, and bucket binding |
| `worker/repair_index.py` | Adds unindexed R2 media to the gallery index |
| `worker/audit_index.py` | Read-only aggregate integrity audit for the live index and bucket |
| `tagtime/` | Resumable Windows JoyTag app, local SQLite state, and tag-index publisher |
| `.github/workflows/yoink.yml` | Scheduled and manual collector |
| `.github/workflows/flush.yml` | Manual index repair |
| `.github/workflows/audit-index.yml` | Manual read-only R2 index audit |
| `.github/workflows/update-cf-web.yml` | Manual Worker deployment |
| `.github/workflows/build-tag-time.yml` | Tested Tag Time Windows ZIP build and rolling release |
| `requirements.txt` | Collector and repair dependencies |

## Collector requirements

- Cloudflare R2 bucket and API credentials
- Cloudflare Worker and deployment token
- GitHub Actions
- A Tailscale exit node available to GitHub Actions
- Reddit cookies in Netscape `cookies.txt` format

## GitHub Actions secrets

Collector, Flush, and R2:

```text
R2_ACCOUNT_ID
R2_ACCESS_KEY_ID
R2_SECRET_ACCESS_KEY
R2_BUCKET_NAME
```

Yoink routing and Reddit:

```text
TS_OAUTH_CLIENT_ID
TS_OAUTH_SECRET
TS_EXIT_NODE_IP
REDDIT_COOKIES_BASE64
REDDIT_SOURCE_1
REDDIT_SOURCE_2
REDDIT_SOURCE_3
REDDIT_SOURCE_4
REDDIT_SOURCE_5
REDDIT_SOURCE_6
REDDIT_SOURCE_7
REDDIT_SOURCE_8
REDDIT_SOURCE_9
REDDIT_SOURCE_10
```

Each optional `REDDIT_SOURCE_*` value may be a subreddit name or a complete Reddit `/new/` URL. These ten slots remain compatible as private fallback sources. New sources can also be added without editing GitHub settings by pressing the gray `+` in the certificate-protected viewer. The Worker stores those additions in the private R2 object `_internal/reddit-sources.json`, and Yoink merges both source sets before downloading.

Worker deployment:

```text
CLOUDFLARE_API_TOKEN
CLOUDFLARE_ACCOUNT_ID
CLOUDFLARE_WORKER_NAME
R2_BUCKET_NAME
```

The Worker also requires its runtime `CONTACT_EMAIL` secret.

## Collector configuration

`settings.json` contains:

```json
{
  "sources": [
    "placeholder1",
    "placeholder2",
    "placeholder3"
  ],
  "browser_user_agent": "Mozilla/5.0 ...",
  "posts_per_subreddit_per_scan": 100,
  "reddit_request_delay_min_seconds": 2,
  "reddit_request_delay_max_seconds": 4,
  "stop_after_consecutive_archived_posts": 15,
  "reddit_429_backoff_seconds": 60,
  "allowed_extensions": ["jpg", "jpeg", "png", "gif", "webp", "mp4", "m4v", "webm"],
  "r2_gallery_prefix": "gallery/",
  "maximum_file_size_mb": 500,
  "download_retries": 4,
  "download_timeout_seconds": 45
}
```

The private `REDDIT_SOURCE_*` secrets replace the placeholder entries. Before Reddit access begins, `app.py` also reads `_internal/reddit-sources.json`, removes duplicates, discards harmless placeholders, and builds one temporary private runtime list. Keep `r2_gallery_prefix` aligned with the Worker and Flush configuration.

## Collector operation

Run **Actions → Yoink → Run workflow**, or allow its schedule to run:

```text
3,18,33,48 * * * *
```

Yoink first merges the optional numbered secrets with sources added through the private viewer. It then restores the archive database, selects the configured private exit node, downloads new media, restores the direct GitHub route, uploads media to R2, conditionally merges additions into `gallery-index.json`, and saves the archive. Source names loaded from R2 are masked before later commands run.

Run **Actions → Flush → Run workflow** after an interrupted upload may have placed media into R2 without updating the index. Flush adds missing valid objects to `gallery-index.json`; it never deletes media. Yoink and Flush use the same ETag-protected read/merge/write helper, so a concurrent writer must retry against the newest index instead of overwriting another writer's additions or removals.

Run **Actions → Audit Index → Run workflow** to compare the live index with R2 without modifying either one. It reports aggregate counts for duplicate keys, malformed metadata, incorrect random weighting, missing objects, and unindexed objects. It never prints media filenames or credentials, and the run turns red when it finds an integrity problem.

Run **Actions → update-cf-web 3 → Run workflow** to publish Worker source changes. Repository commits do not automatically deploy the live Worker.

Workflow display names end with a revision number. Increment that number whenever the corresponding workflow file changes. `update-cf-web 3` explicitly checks out current `main`, records the exact deployed commit in the run summary, and verifies both the protected source manager and Tag Time contract before deploying. Historical deployment revisions must not be rerun.

## Private viewer source manager

The certificate-protected viewer has a transparent gray `+` centered between the media filter and the GitHub/email links. Press it, enter a subreddit name such as `pics`, and press **Add**. The same box also accepts `r/pics` and a complete Reddit subreddit URL.

The browser submits `POST /api/sources` as a same-origin background JSON request. The endpoint requires a successfully verified, non-revoked mTLS client certificate, the application-only `x-gparty-source-request` header, JSON content, and exactly one small `subreddit` field. Cross-site HTML forms cannot create that request, and cross-origin scripts cannot pass the browser's CORS preflight. The endpoint validates and normalizes the name, rejects malformed input, ignores case-insensitive duplicates, and uses an ETag-protected R2 write so simultaneous additions cannot silently overwrite each other. It returns a compact JSON result to the existing dialog without navigating away from the viewer. The private source object is outside `gallery/`, is absent from `gallery-index.json`, and cannot be served by the media route. Yoink reads it on the next scheduled or manual run.

Worker deployments set both `workers_dev = false` and `preview_urls = false`, preventing alternate public Worker URLs from bypassing the custom hostname's certificate protection.

## GParty Tag Time

Tag Time locally classifies the R2 library with [JoyTag](https://github.com/fpgaminer/joytag), a multi-label Danbooru-style model designed for illustrated and photographic media. The Windows app uses DirectML for the RTX GPU without requiring a separate CUDA toolkit. It samples still images plus representative GIF and video frames.

Download [the latest Tag Time Windows ZIP](https://github.com/polskiftw/gparty/releases/download/tag-time-windows-latest/GParty-Tag-Time-Windows.zip), extract it, copy `config.example.txt` to `config.txt`, and fill in these four R2 values:

```text
R2_ACCOUNT_ID=
R2_ACCESS_KEY_ID=
R2_SECRET_ACCESS_KEY=
R2_BUCKET_NAME=
```

No Cloudflare dashboard changes or separate credentials are required. The R2 token needs object list/read/write access. Press **TAG TIME**. The first run downloads the official JoyTag model once. The app stores completed work in `data/tag-time.sqlite3`, retries only failed/new/changed assets, and uploads `_internal/tag-index-v1.json` every 100 completed files and at a clean stop.

The Worker exposes only tag names and individual counts to the certificate-protected viewer. Desktop renders the tag catalog in a left sidebar. Checked tags use AND matching; all unchecked means the original fully random behavior. Mobile omits the sidebar entirely. The private item-to-tag mapping cannot be reached through `/media/`.

## Built with

- [gallery-dl](https://github.com/mikf/gallery-dl)
- [yt-dlp](https://github.com/yt-dlp/yt-dlp)
- [Boto3](https://github.com/boto/boto3)
- [OpenCV](https://opencv.org/)
- [Pillow](https://python-pillow.org/)
- [PyInstaller](https://pyinstaller.org/)

## License

GParty is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE). Commercial use requires prior written permission from the copyright holder.

Copyright © 2026 polskiftw.
