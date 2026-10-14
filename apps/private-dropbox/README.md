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
so multi-gigabyte exports may be slow or rate-limited. The browser must
stay open for the export; reopening the page starts another attempt.
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
overwriting data. Multipart sessions are cancelled on a known error; orphaned
upload parts expire by the R2 lifecycle policy. Retry of individual failed
parts is supported while the browser page is still open.

No rename, move, delete UI, persistent restart-after-refresh upload recovery,
folder-tree drag/drop or automatic binary decompilation in the first version.
R2 listings currently cap results at 1,000 per segment and show a warning
when the listing was truncated.

## Test

    node --test apps/private-dropbox/tests/*.test.mjs

PRs also run a Wrangler no-deployment bundle validation. Deployment is manual.
