# gfingerd

`gfingerd.exe` is GParty's quiet, read-only Windows fingerprint daemon. It
maintains the shared exact-version fingerprint registry for the canonical
Backblaze B2 media library while boink continues acquiring new media normally.
The name is intentionally daemon-style: **gfingerd**.

## One-EXE setup and cold-boot behavior

Double-click `gfingerd.exe` to open its raw Win32 control panel. Normal
configuration lives in that GUI: the dedicated read-only B2 login, bucket,
canonical prefix, inventory interval, fingerprint worker count, download
connection count, prefetch-file capacity, and machine-boot setting.

**Save & Start** validates the read-only key and performs a one-time elevated
self-install/update. The installed copy is `%ProgramFiles%\GParty\gfingerd.exe`.
Task Scheduler launches it with `ONSTART` as `SYSTEM` at highest privileges, so
it starts when the PC powers on, before Windows sign-in and without needing the
user's PIN. Opening the EXE later opens only the control panel; the global
worker mutex prevents a second background daemon.

Upgrades from the earlier background-fingerprinter build are adopted rather
than duplicated. The GUI can read the previous user config and Credential
Manager entry, and installation removes the legacy Task Scheduler entry and
old `%ProgramFiles%\GParty\gparty-fingerprinter.exe` copy after stopping it.

## Persistent downloads and bounded prefetch

Each network-owning B2 client owns one long-lived libcurl easy handle. Requests
use `curl_easy_reset()` between operations, which clears request-specific
options while retaining libcurl's connection/DNS/TLS session caches. Normal
HTTP connection reuse stays enabled and TCP keepalive is enabled. The daemon
therefore reuses an existing B2 TCP/TLS connection across successive files
when the server keeps that connection available instead of intentionally
tearing it down at every file boundary.

Downloading and fingerprinting are separate pipeline stages:

1. Configured **Download connections** independently claim pending objects and
   download exact B2 file IDs to `.partial` staging files.
2. Size and B2 SHA-1 are verified before a staged file is renamed into a ready
   item. Fingerprint workers never see a partial transfer.
3. Successfully downloaded files enter a bounded ready queue. **Prefetch
   files** is the queue's maximum file count; producers block when it is full,
   so the queue cannot grow without bound.
4. Configured **Fingerprint workers** consume ready files while downloaders
   continue filling newly freed queue slots.
5. After decoding/fingerprinting, gfingerd performs a final exact-object B2
   lookup. A result is persisted only if key, file ID, size, and available
   SHA-1 still identify the same object version. The staged file is then
   removed.

This keeps network activity much smoother than the old download-then-decode
per-worker loop, particularly for many small files. The GUI and **Live CMD
Output** report active/configured downloads, aggregate download speed,
ready/configured prefetch slots, active/configured fingerprint workers, session
bytes, completion/failure counts, and current files.

The defaults are four download connections, eight prefetched ready files, and
two fingerprint workers. GUI validation bounds download connections and
fingerprint workers to 1-16 and prefetch files to 1-64.

## Early boot and NVDEC

JPEG, PNG, WebP, and GIF work is independent of NVIDIA NVDEC and continues at
cold boot even when the display driver is not ready yet. Only MP4, M4V, and
WebM require NVDEC. NVDEC-dependent video is filtered before the download and
prefetch stage while the driver is unavailable, remains pending without
consuming its failure budget, and is rechecked every 15 seconds.

The live status records both daemon launch time and the first successful NVDEC
probe, making the pre-sign-in startup path visible after login.

## B2 permissions and failure safety

The B2 surface is deliberately read-only: list, exact lookup, and exact-version
download. The application key needs `listFiles` and `readFiles`, plus
`listBuckets` only when it is not restricted directly to the configured bucket.
Restricting it to the canonical `gallery/` prefix is recommended.

Transient B2/network/service failures do not consume a media object's durable
failure budget. Corrupt or unsupported media remains isolated to that object
and cannot stop the backlog. Clean shutdown cancels active libcurl transfers,
wakes blocked queue producers/consumers, removes incomplete `.partial` files,
and removes any downloaded ready files that were not consumed.

## Existing fingerprint corpus

On its first reconciled run, gfingerd can adopt compatible gdupe v3 fingerprint
components from `%LOCALAPPDATA%\gdupe\gdupe.sqlite3` after validating each row
against the live B2 key, file ID, size, and SHA-1 where available. The legacy
gdupe database is read-only and is never modified.

The registry itself is `%LOCALAPPDATA%\GParty\fingerprints.sqlite3`. SQLite uses
WAL, `synchronous=FULL`, foreign keys, and completed-fingerprint transactions.
Staging lives under `%LOCALAPPDATA%\GParty\fingerprint-cache\` using a digest of
the B2 file ID.

### Deferred malformed GIFs

The known malformed-GIF geometry case discovered by the live `giftest` sweep is
intentionally deferred rather than repeatedly retried. When WIC can open a GIF
but its decoded frame rectangle disagrees with the declared logical canvas,
`gfingerd` does not fingerprint that file yet. After re-verifying the exact B2
file identity, it moves the downloaded original into
`%LOCALAPPDATA%\GParty\deferred-gifs\`, writes a JSON recovery note beside the
GIF, and records the exact file ID, local path, and decoder reason in the
`deferred_gifs` registry table. That exact object version is excluded from
normal pending work and appears as its own GUI/stat count. A replacement B2
version does not inherit the deferral.

These files are deliberately recoverable. `TODO.md` keeps the proper malformed
GIF normalization/reprocessing work as the first outstanding item; once that
is implemented, the registry records and saved originals provide the reprocess
queue.

New installations use `%LOCALAPPDATA%\GParty\gfingerd.json` and
`%LOCALAPPDATA%\GParty\gfingerd.log`. Runtime status is published atomically as
`gfingerd-status.json` beside the log. An earlier `fingerprinter.json` and
Credential Manager entry are accepted as migration inputs.

## Media and distribution boundary

The EXE shares gdupe's native fingerprint/decode implementation:

| Media | Decoder path |
| --- | --- |
| JPEG | statically linked libjpeg-turbo |
| PNG | Windows Imaging Component |
| WebP | statically linked libwebp |
| GIF | Windows Imaging Component |
| MP4/M4V | native minimp4 demux + NVIDIA NVDEC |
| WebM | native libwebm demux + NVIDIA NVDEC |

The GUI is raw Win32. FFmpeg, Media Foundation, Qt, FLTK, Electron, and .NET are
not used. Redistributable libraries and the MSVC runtime are statically linked,
and the package audit requires zero shipped DLLs.

Advanced troubleshooting flags remain available: `--once`, `--status`,
`--set-credentials`, and `--daemon`. `--boot-worker`, `--install-boot`, and
`--uninstall-boot` are internal self-install/Task Scheduler operations.
