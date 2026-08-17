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

## One-EXE experience

Double-click `gparty-fingerprinter.exe`. Its Windows control panel contains the
complete normal configuration: the dedicated read-only B2 login, bucket,
canonical prefix, inventory interval, worker-thread count, and start-at-login
setting. **Save & Start** validates the key, stores it in Windows Credential
Manager under `GParty/fingerprinter-b2`, saves non-secret settings under
`%LOCALAPPDATA%\GParty`, and starts or safely restarts the background worker.
No installer, adjacent configuration file, terminal command, or administrator
access is required.

At Windows login, Task Scheduler launches that same EXE with its internal
background mode 30 seconds after sign-in. It has no visible window and runs at
below-normal process priority. Opening the EXE yourself never creates a second
worker; it opens the control panel instead.

The control panel refreshes live library and session statistics, including
configured/active workers, aggregate download speed, session download volume,
current objects, completed coverage, pending work, failures, and unsupported
objects. **Live CMD Output** opens a separate command window that follows the
rotating operational log and prints a current throughput summary every five
seconds. Closing either the control panel or that command window does not stop
the background worker.

Advanced troubleshooting flags remain available: `--once`, `--status`,
`--set-credentials`, `--install-autostart`, and `--remove-autostart`. For
temporary automation, both `GPARTY_FP_B2_KEY_ID` and
`GPARTY_FP_B2_APPLICATION_KEY` may be set in the environment.

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
`%LOCALAPPDATA%\GParty\fingerprinter.log`; transient live process statistics are
published atomically beside it in `fingerprinter-status.json`. `--status`
reports current inventory,
coverage, pending components, unsupported/failed counts, last successful scan,
and the object currently being processed.

## Media and distribution

The executable uses the same shared native fingerprint/decode target as gdupe,
preserving gdupe fingerprint version 3 output for JPEG, PNG, WebP, GIF, MP4,
M4V, and WebM. Video decoding uses the installed NVIDIA driver through NVDEC;
FFmpeg is not used or shipped. Redistributable libraries and the MSVC runtime
are statically linked, and the application package contains no DLL pile.
