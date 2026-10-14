# Private Debug Zone - Design A (GitHub bridge)

Adds a friend-only shared file browser to the existing Cloudflare Worker for gooning.party.
No universal files subdomain, no accounts, and no user-separated folders.

**Deployment is opt-in, not automatic.** The current gallery stays as-is until the
Cloudflare bucket, private GitHub repo, certificate rule, and secrets are provisioned.

## Features

- Root-domain direct URLs like /Player.log or /Valheim/Crash/Player.log.
- Clickable folders, drag/drop multi-file upload, link copying, and text previews.
- Everybody with an authorized client certificate has the same access.
- File contents live in a *separate private R2 bucket*, never the public GitHub source repo.
- Sharing with ChatGPT is explicit per text file. After confirming, a private
  GitHub copy is committed at shared/the/same/path so the GitHub connector can read it.
- GitHub repository privacy is checked on *every* share; public repos are refused.
- Existing gallery remains available under /gallery/ in private mode, while its
  existing internal API and media routes remain reserved and operational.
- Enabling private mode guards *all* site resources with Cloudflare-verified mTLS.
- No unauthenticated previews, direct downloads, upload APIs, or thumbnails.

### First-version limits

Uploads: 64 MiB per file; the request must have Content-Length.
AI sync: UTF-8 text/code/log files up to 4 MiB per file. Binary dumps stay in R2.
No folder-tree upload, delete UI, rename/move, or overwrite yet. Existing names
are never silently replaced. R2 directory listing is limited to 1000 results
per segment, and the UI warns when a listing was truncated.

**Privacy note:** Github retains file contents in commit history, including after
a normal deletion. Review logs for secrets, tokens, IPs, personal data, etc.
Only the specifically selected files are copied to GitHub, but the repository's
owner, GitHub, and the connected ChatGPT GitHub app can inspect those copies.

## Provisioning checklist (user action required)

1. Create a NEW **private** GitHub repository, e.g. polskiftw/gparty-private-debug.
   Never use the public gparty or gpages repo for any logs. Your friend does not
   need a GitHub account. Give your connected ChatGPT GitHub app access to this
   new repository in its installation settings.
2. Create a fine-grained GitHub token scoped ONLY to that private repository, with
   Contents: Read and write (and automatically included Metadata read).
   Store it as the **Cloudflare Worker secret AI_GITHUB_TOKEN**, not as a repo
   file, browser setting, or Actions secret.
3. Create a **new private R2 bucket** and keep its public r2.dev URL and any public
   custom domain disabled. Do not reuse the gallery's MEDIA_BUCKET.
4. Issue a verified client certificate per device/friend. Configure Cloudflare
   mTLS for the ENTIRE gooning.party hostname, not merely the upload endpoint.
   Recommended Cloudflare WAF custom rule action BLOCK:

       (not cf.tls_client_auth.cert_presented) or (not cf.tls_client_auth.cert_verified) or cf.tls_client_auth.cert_revoked

   The Worker independently checks presented, verified and non-revoked status.
   The WAF rule is defense in depth against accidentally restoring a public Worker.
5. In GitHub Actions secrets for polskiftw/gparty, configure:
   - PRIVATE_FILES_R2_BUCKET: the name of the new R2 bucket
   - AI_GITHUB_REPO: private repo name in owner/repository format
   Retain the existing Cloudflare deployment and gallery secrets.
6. In GitHub Actions repository variables, set
   GPARTY_PRIVATE_DROPBOX_ENABLED = true (exact text).
7. Manually run the existing update-cf-web workflow. It binds both buckets and
   enables the new homepage. The site, including the former gallery, becomes
   certificate-only and the gallery moves to /gallery/.

Cloudflare Worker secrets and GitHub Actions secrets are separate. The worker
secret AI_GITHUB_TOKEN is never sent to a browser and not committed in public
source. The deployment workflow refuses an insecure downgrade if it sees the
private R2 bucket configured without the private-mode variable enabled.

## Paths

Private R2 keys:
- files/FOLDER/FILE -> browser /FOLDER/FILE
- folders/FOLDER/ -> empty-directory marker
- ai-shares/FOLDER/FILE -> marker for an AI copy matching the current file ETag

Private GitHub keys:
- shared/FOLDER/FILE -> text explicitly selected for AI access

The internal /_/ namespace is reserved for private file-manager API/assets.
Existing /gallery/, /api/, /media/, style and favicon paths are reserved so
the old media gallery continues working. File paths must be created through
the site and are otherwise ordinary direct URLs.

## Testing

Node 22:
    node --test apps/private-dropbox/tests/*.test.mjs

The CI check also runs a Wrangler dry-run build; it does not deploy anything.
