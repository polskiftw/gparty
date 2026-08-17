# GParty background fingerprinter

`gparty-fingerprinter.exe` is the quiet, read-only fingerprint maintenance
process for GParty's canonical Backblaze B2 media library. It continuously
records exact-content, visual, crop, and moving-media timeline fingerprints in
`%LOCALAPPDATA%\GParty\fingerprints.sqlite3` while boink continues acquiring
media normally.

The executable has no B2 upload, hide, copy, or delete API. Its application key
needs only `listFiles` and `readFiles`, plus `listBuckets` when the key is not
restricted directly to `gooning-party-media-b2`. Restricting the key to the
`gallery/` prefix is recommended.

## First run

Extract the Windows artifact, open a terminal in the extracted directory, and
run:

```powershell
.\gparty-fingerprinter.exe --set-credentials
.\gparty-fingerprinter.exe --once
.\gparty-fingerprinter.exe --status
.\gparty-fingerprinter.exe --install-autostart
```

The first command validates and stores the dedicated read-only B2 login in
Windows Credential Manager under `GParty/fingerprinter-b2`. For temporary
automation, set both `GPARTY_FP_B2_KEY_ID` and
`GPARTY_FP_B2_APPLICATION_KEY` instead.

`--once` performs one inventory/adoption/catch-up pass in the foreground.
Without `--once`, the process stays active and checks for new canonical media
every ten minutes. The installed per-user Task Scheduler entry starts it 30
seconds after logon, hidden and at below-normal process priority. Remove that
entry with:

```powershell
.\gparty-fingerprinter.exe --remove-autostart
```

## Existing fingerprint corpus

Before computing broad catch-up work, the first reconciled run opens
`%LOCALAPPDATA%\gdupe\gdupe.sqlite3` read-only. Matching gdupe v3 rows are
validated against the live B2 key, file ID, size, and SHA-1 where available.
Each structurally valid compatible component is adopted independently, so
already-completed fingerprinting remains completed and partial rows require
only their missing pieces.

The old gdupe database is never changed. Adoption uses idempotent component
keys and commits in bounded batches, so interruption and restart are safe.

## Registry and safety

Fingerprint components have independent algorithm versions:

- `media_info`
- `sha256`
- `phash64`
- `perceptual256`
- `crop_phash64`
- `timeline_phash64` for GIF/video

SQLite uses WAL, `synchronous=FULL`, foreign keys, and completed-fingerprint
transactions. A result is saved only when the registry and a final B2 lookup
still identify the exact file version that was downloaded. Replaced or removed
objects are discarded and reconciled later. Unsupported objects are terminal
for that version; corrupt media receives durable bounded retries and cannot
stop the remaining backlog.

Downloads are staged under
`%LOCALAPPDATA%\GParty\fingerprint-cache\` using a file-ID digest, then removed
after persistence. Only orphan `.partial` files inside that dedicated directory
are cleaned at startup.

The rotating operational log is
`%LOCALAPPDATA%\GParty\fingerprinter.log`. `--status` reports current inventory,
coverage, pending components, unsupported/failed counts, last successful scan,
and the object currently being processed.

## Media and distribution

The executable uses the same shared native fingerprint/decode target as gdupe,
preserving gdupe fingerprint version 3 output for JPEG, PNG, WebP, GIF, MP4,
M4V, and WebM. Video decoding uses the installed NVIDIA driver through NVDEC;
FFmpeg is not used or shipped. Redistributable libraries and the MSVC runtime
are statically linked, and the application package contains no DLL pile.
