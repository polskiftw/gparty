# Private Debug Zone - deployment and operations

The new gooning.party is ONE shared private file directory, not the former
R2 media gallery. Every request requires verified, non-revoked client mTLS.
The original media application is retired from the Worker and its scheduled
acquisition workflow is removed. Do not delete any old bucket until you are
certain its contents are no longer needed.

## What the site does

- Root URL is the file browser; real files live at /name.ext or /folder/name.ext.
- Directories are created in the website; only /_/ is reserved internally.
- Drag/drop multiple files, or click to select files; copy absolute links.
- Up to 64 MiB use a single R2 streaming request. Files above 64 MiB use
  16-64 MiB authenticated multipart parts, with up to three parallel
  transfers and retries per part. Maximum current file size is 625 GiB.
- R2 private bucket stores the originals; the site is not backed by Git.
- UTF-8 logs/code are previewed in-browser with line numbers.
- Potentially executable formats such as HTML are forced downloads.

## Optional GitHub AI bridge

Only when someone presses Share with AI and confirms does the app copy data
to a SEPARATE PRIVATE GitHub repository. The destination is checked for
privacy on every operation. No binary dump or ZIP is exported automatically.

Text exports use small 512 KiB chunks to preserve compatibility with
ChatGPT's private GitHub connector. Source ETags guard against uploading
pieces from two different versions of the same log. After all chunks are
stored as Git blobs, one Git commit publishes them and an index together.
The AI READY marker is written to R2 only if that commit succeeds.

There is no arbitrary 4 MiB limit anymore. Extremely large logs still
encounter GitHub API quota, blob-creation and repository-size constraints,
so multi-gigabyte exports may be slow or rate-limited. The browser stores completed chunk receipts in sessionStorage. Return to the
same tab and press Share with AI again to resume an interrupted export.
GitHub stores history even if a file is later deleted. Review logs for
private details before sharing.

## Provisioning: required before first deployment

1. Create a NEW private GitHub repository, e.g.
   polskiftw/gparty-private-debug (different from public gparty/gpages).
   Initialize it with a default main branch (README is fine). Give the
   connected ChatGPT GitHub app read access through its installation
   repository-selection settings. Friends do not need GitHub accounts.
2. Create a fine-grained GitHub PAT with Contents: read/write, restricted
   ONLY to the new private repo. Store it as a CLOUDFLARE WORKER SECRET
   named AI_GITHUB_TOKEN. This token is never committed or sent to a browser.
3. Create a new private Cloudflare R2 bucket such as gparty-private-files.
   Disable public R2 URLs and custom-domain/public access for the bucket.
   Do NOT reuse the previous gallery or Backblaze B2 storage.
4. Issue separate mTLS client certificates to each trusted device/friend.
   Configure Cloudflare hostname-wide verified client certificate policy
   and a WAF rule BLOCKING requests without valid, non-revoked certificates:

       (not cf.tls_client_auth.cert_presented) or (not cf.tls_client_auth.cert_verified) or cf.tls_client_auth.cert_revoked

   The Worker also rejects unauthorized connections, including to direct
   files, uploads, JS/CSS assets, previews and API routes. Set the edge rule
   BEFORE deployment as protection against accidentally deploying a
   different public Worker later.
5. In GitHub Actions repository SECRETS for polskiftw/gparty configure:
   - PRIVATE_FILES_R2_BUCKET = name of the new private R2 bucket
   - AI_GITHUB_REPO = owner/new-private-repo
   - CLOUDFLARE_WORKER_NAME, CLOUDFLARE_ACCOUNT_ID, CLOUDFLARE_API_TOKEN
     = existing Cloudflare deploy configuration, with appropriate scopes
6. In repository VARIABLES set GPARTY_PRIVATE_DROPBOX_ENABLED = true.
7. Trigger the manual GitHub Actions workflow deploy-private-file-site.

The deployment workflow refuses to run without the explicit opt-in and
required settings. The live domain is NOT altered merely by merging code.
There are no new public alternate domains or workers.dev preview URLs.

## Browser behavior

On upload name collisions the site rejects the second file, instead of
overwriting data. Multipart receipts are kept in browser sessionStorage. If a connection is
interrupted or the page reloads, select the same original file to resume
remaining parts in the same tab. R2 automatically aborts incomplete multipart
uploads after its lifecycle timeout (default seven days). Configure an R2
lifecycle rule to clean stale multipart-sessions/ metadata markers as well.

No rename, move, delete UI, cross-browser persistent uploads, folder-tree
drag/drop, or automatic binary decompilation in the first version.
R2 listings currently cap results at 1,000 per segment and show a warning
when the listing was truncated.


## Troubleshooting folder creation or uploads

Every JSON API response includes an identifying X-GParty-API: 1 header.
The browser reports the request method, path, HTTP code, optional HTML page
title, and Cloudflare Ray ID if an intermediary serves HTML instead of JSON.
An actual R2 failure handled by GParty returns JSON with a Worker error ID.

If a folder creation attempt shows:

- HTTP 403 **without** an X-GParty-API response: check the matching Ray ID in
  Cloudflare zone Security > Events. This may be a WAF block or challenge.
  Do NOT turn off hostname-wide mTLS. Investigate the specific triggering
  rule and only adjust it if the event confirms a false positive.
- HTTP 1101 / Worker runtime error: Cloudflare Workers & Pages > the deployed
  Worker > Logs. Filter for exception events.
- HTTP 503 **with** a Worker error ID: find that ID in the Worker logs to
  identify which R2 operation failed.
- HTML/404 response: verify that Cloudflare Workers & Pages > the intended
  Worker > Settings > Domains & Routes actually routes gooning.party to the
  Worker. An upload that reports 'No deploy targets' in Wrangler does not
  itself establish a hostname route.

The browser console also logs the HTTP details of a non-JSON response.
Do not paste certificate private keys, API tokens or passwords into reports.

## Test

    node --test apps/private-dropbox/tests/*.test.mjs

PRs also run a Wrangler no-deployment bundle validation. Deployment is manual.

## Original MIDI, cursor effects and visitor display

The optional eye-searing arcade layer only changes the page and serves
local assets; it does not change R2 upload, listing, preview or AI export
behavior. `/_/internet-anthem.mid` is a playable standard MIDI file and
also drives the browser's simple square/triangle-wave synthesizer.

A new `VISITOR_COUNTER` Durable Object binding (SQLite-backed, migration
`gparty-visitor-counter-v1`) tracks the real total visits and recently
active tabs. Only an anonymous short-lived random tab ID is used. If the
binding is missing, the display fails softly and private file operations
remain available. The Worker rejects counter updates from clients without
verified mTLS or without same-origin write permission.

Counter traffic and all decorative assets stay under the protected `/_/`
namespace. To roll out the change, merge then manually run the existing
private-site deployment workflow; it includes the DO migration and requires
the same Cloudflare security and bucket secrets as before.

## Reddit Post Freezer 3000 - cookie-authenticated HTML mode

The form on every folder page submits an opaque job to a **temporary GitHub
Actions runner**, not to a Reddit API. The private post URL is stored in
`archive-jobs/<job-id>/request.json` inside the existing private file R2
bucket; the public GitHub Actions dispatch carries **only the random job ID**.
No Reddit user name, post URL, or cookies are passed as workflow inputs.

The runner uses your prior Tailscale connection (`tailscale/github-action@v4`)
and your existing **home exit node**, verifies the egress IP changed before
contacting Reddit, and fails closed if the route cannot be established. It
always loads the saved Firefox Reddit cookie export from
`REDDIT_COOKIES_BASE64` and sends a Firefox User-Agent, even for public
posts. It fetches the authenticated Reddit **HTML** page, extracts
post content/gallery media URLs, and downloads direct Reddit CDN files.
For Reddit-native video, it may give yt-dlp a *direct v.redd.it DASH
manifest*, which can mux separate audio/video into a playable MP4.
No Reddit OAuth, Reddit app ID/secret, or Reddit REST/JSON endpoint is used
by our fetcher. Do not replace the HTML URL with `.json` or pass a Reddit
post URL to yt-dlp (that would invoke its Reddit API extractor).

The runner creates `reddit-<post-id>-<title>.tar.zst` with:
- `post.json` - metadata extracted from post HTML, title and body when exposed;
- `post.md` - readable post text and source link;
- `media/` - best-effort gallery images and native Reddit video;
- `ARCHIVE_NOTES.txt` - captured count, limitations and media failures.

It uploads the result to the same private R2 bucket under the job prefix
and marks it ready. The browser polls GParty over existing mTLS, then
automatically downloads `/_/api/archive/file?id=<job-id>`. The Worker
never serves archived files without verified mTLS. Existing file-manager
uploads and other features are unchanged. Configure R2 lifecycle expiration
for `archive-jobs/` (for example after two days), because they are
temporary and are not part of the normal file listings. The site stops
serving job results after 24 hours.

### One-time setup

1. Reuse existing GitHub Actions secrets **if still present and valid**:
   `TS_OAUTH_CLIENT_ID`, `TS_OAUTH_SECRET`, `TS_EXIT_NODE_IP`,
   `REDDIT_COOKIES_BASE64`, `R2_ACCOUNT_ID`, `R2_ACCESS_KEY_ID`,
   `R2_SECRET_ACCESS_KEY`, `PRIVATE_FILES_R2_BUCKET`.
   The R2 access key must grant read/write to the same private file
   bucket currently bound to the GParty Worker, **not the old gallery bucket**.
   The `PRIVATE_FILES_R2_BUCKET` deployment secret already identifies
   the modern bucket; a credential with access to it may still be needed.
2. Create a fine-grained GitHub personal access token for **only**
   `polskiftw/gparty`, with repository **Actions: read/write**
   permissions. Add it as a **Cloudflare Worker secret** called
   `ARCHIVE_GITHUB_TOKEN` (not a GitHub repository secret).
   This allows the Worker to dispatch `archive-reddit-post.yml`;
   it does not give the browser any GitHub token.
3. Optional: set GitHub Actions repository variable `REDDIT_BROWSER_UA`
   to the Firefox UA matching the cookie-exported profile; otherwise
   the runner uses the default Firefox UA recorded in its source.
4. Merge the tested changes, then run the existing guarded **manual**
   `deploy-private-file-site` workflow. No site deployment happens on merge.

**Cookie freshness:** Firefox cookies cannot be read by a Cloudflare Worker
or a GitHub runner without being transferred. This integration **always**
uses the existing `REDDIT_COOKIES_BASE64` secret; it does not automatically
read new cookies from your PC. If the saved session expires or is revoked,
re-export your authenticated Firefox reddit.com cookies to Netscape
`cookies.txt` and update that GitHub Actions secret. Do not send the cookie
file to ChatGPT or commit it.

**Completeness limits:** HTML markup changes frequently and Reddit may
reject automated browsing. Some galleries load assets dynamically or
third-party hosts, and external media such as RedGIFs are not guaranteed.
The initial HTML extractor is best-effort and does **not** guarantee every
media attachment on every post. When expected media cannot be found or
downloaded, the tool reports a failure or includes a warning instead of
silently claiming the post was completely archived. No comments are included.

The Worker must not try to use the Tailscale router directly; it cannot
run Tailscale. This design reuses the old ephemeral-runner routing so the
home PC can stay switched off.
