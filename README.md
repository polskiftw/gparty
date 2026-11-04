# GParty: Private File Drop and Debug Workspace

**gooning.party is now a certificate-only shared file playground.** The former
random gallery and its media acquisition/publishing pipeline are retired.
This repository contains the Cloudflare Worker source for the replacement.

Friends share one filesystem. Anyone with an authorized mTLS client certificate
can browse, create folders, drop files, preview logs, and copy direct links.
Files live at the root domain:

- https://gooning.party/Player.log
- https://gooning.party/Valheim/Crash/Player.log
- https://gooning.party/random.zip

The homepage itself is the root directory. Folder URLs end in a slash.
Only the internal /_/ namespace is reserved for file-manager assets and APIs.
There is no files subdomain or universal /files/ URL prefix.

## Architecture

    Firefox with valid mTLS client certificate
                 |
          Cloudflare Worker
                 |
      Private R2 full-size originals
                 |
          Optional approval
                 |
     Text chunks -> Private GitHub repo
                 |
      ChatGPT GitHub connector

### Features

- Drag-and-drop multi-file uploads, browser file chooser, progress display.
- Normal single-request uploads up to 64 MiB. Larger files automatically use
  R2 multipart uploads with 16-64 MiB parts and retries.
- Current multipart design supports up to 625 GiB per file (64 MiB x 10,000
  parts), subject to actual storage and cloud plan constraints.
- Direct file links remain accessible only to holders of valid client certificates.
- Shared folder browsing/creation, text previews with line numbers, and safe
  forced downloads for potentially executable HTML uploads.
- Only explicitly approved UTF-8 text logs/code are exported to the separate
  private GitHub destination.
- Large text exports create independently readable ~512 KiB chunks and a
  manifest in one final private Git commit, rather than hundreds of commits.
- The target GitHub repository is verified PRIVATE for each export operation.
- The file is marked AI READY only after final publication.

## GitHub's real limits

R2 is primary storage. GitHub is only the opt-in debugging bridge.

GitHub REST APIs, repository size guidance and secondary creation rate limits
make massive text-file sync expensive and sometimes slow. The app no longer has
the former arbitrary 4 MiB AI cap, but exports still depend on GitHub quotas and
the ability to process many small chunks. Large logs may require inspecting only
selected parts, not one monolithic output. Binary crash dumps and archives stay
in R2 because a text connector cannot automatically analyze arbitrary binaries.

**Privacy warning:** GitHub stores previous file versions in commit history;
deleting a file does not necessarily remove earlier content. Review logs for
private tokens, passwords, IP addresses and identifying data before AI export.
GitHub and the connected ChatGPT GitHub app can read those approved copies.

## Reddit post downloads (.tar.zst)

The GParty homepage provides the Reddit Post Freezer 3000: paste one Reddit
post URL and click Save .tar.zst. The browser receives a TAR/Zstandard archive
containing the post metadata/text and available media after completion.

**Clean separation:** this PUBLIC repo contains the mTLS website, Cloudflare
Worker, and the private R2 request/status/download API only. The downloader,
its credentials, Tailscale runner, gallery-dl and ffmpeg live entirely in
the PRIVATE repo [polskiftw/private-debugging-logs](https://github.com/polskiftw/private-debugging-logs).
Only a random UUID is dispatched to the private repo; post URLs stay inside
private R2. No cookie or downloaded media goes into GitHub commits/logs.

The ephemeral private GitHub runner uses the existing Flint 2 Tailscale exit
node and ALWAYS uses the saved Firefox Reddit login cookies (including for
public posts). It uses gallery-dl with reddit.api=rest, which makes Reddit
REST/JSON calls using login cookies **without a Reddit OAuth developer app**.
This is not a custom HTML scraper and no local daemon is required.

**One-time setup:** create a fine-grained GitHub token scoped to ONLY the
PRIVATE repo with Actions read/write; save it as Cloudflare Worker secret
ARCHIVE_GITHUB_TOKEN. Configure matching Tailscale, Reddit-cookie and R2
Actions secrets in the PRIVATE repo. The exact names and steps are documented
in the private repo's docs/REDDIT_ARCHIVER_SETUP.md.

The existing manual deploy-private-file-site workflow deploys the website
after both repositories are configured. Full authenticated downloads require
an actual valid cookie and live test; passing unit tests alone cannot
guarantee all Reddit posts or third-party media work.

## Setup and deployment

See apps/private-dropbox/README.md for the one-time security and provisioning
steps. The manual deploy workflow is:

    .github/workflows/deploy-private-file-site.yml

Deployment needs an isolated private R2 bucket, per-friend mTLS certs and a
hostname-wide protection rule, a separate private GitHub repo, and the
Cloudflare/GitHub secrets. It refuses to deploy unless the GitHub Actions
variable GPARTY_PRIVATE_DROPBOX_ENABLED is exactly true.

Existing gallery code and APIs are not part of the new Worker. This change does
not automatically delete old B2/R2 buckets or media backups.

### Current source tree

- apps/private-dropbox/ - UI, large uploads, opt-in AI bridge, tests
- apps/web/ - Cloudflare Worker entrypoint and Wrangler config
- apps/email-worker/ - independent email-routing Worker (preserved)
- .github/workflows/ - private-site tests and manual deployment

Obsolete media acquisition and gallery tools are no longer built or deployed.
Git history retains the older implementation.

### Tests

    node --test apps/private-dropbox/tests/*.test.mjs

PR checks also run a Wrangler dry-run bundle build.

## License

Apache License 2.0. See [LICENSE](LICENSE).

## Certifiedly unnecessary interface extras

The private file browser also features an original, downloadable **type-1 MIDI**
(`/_/internet-anthem.mid`) performed in the browser using a deliberately
tiny, dependency-free Web Audio chiptune synthesizer. Modern browsers prohibit
unprompted audio; playback begins on the first eligible user interaction (or
when PLAY is pressed), with an obvious SILENCE button. Audio does not affect
uploads, previews, certificates, or the GitHub bridge.

It has a pixelated custom mouse cursor, animated cursor-star trail, original
SVG alien art, a real hit counter, a 60-second-window online count, moving
marquee, digital clock, fortune dispenser, parody web badges, and a button to
make the homepage even more ridiculous. Animated effects honor
`prefers-reduced-motion`; touch devices retain their normal pointers.

The counter is a single **SQLite-backed Cloudflare Durable Object** bound as
`VISITOR_COUNTER`. Hits and a short-lived, random tab ID (60-second expiry)
are stored there. No IP addresses, names, cookies, user agents, or uploaded
files are stored by this feature. The counter is only callable behind the
existing verified mTLS/same-origin write gate, and will display OFFLINE if the
optional namespace fails; the file manager still functions.

The manual deployment configuration includes the namespace binding and
`gparty-visitor-counter-v1` SQLite migration. The site is intentionally
**not** deployed on merge: run the existing guarded manual deployment after
merging and confirm the Cloudflare token has Durable Object deployment
permissions. The first successful view begins at 1 rather than fabricating
a legacy hit history. All visual assets are local and require no third-party
hosts or fonts.
