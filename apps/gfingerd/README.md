# gfingerd

`gfingerd` is GParty's standalone Windows fingerprint daemon. It continuously keeps the existing gdupe SQLite inventory fingerprinted so gdupe can concentrate on duplicate review, preview, and deletion instead of spending foreground time generating fingerprints.

## Data ownership

There is exactly one fingerprint database: the existing gdupe database, normally:

```text
%LOCALAPPDATA%\gdupe\gdupe.sqlite3
```

`gfingerd` does not migrate, copy, import, or maintain a second fingerprint registry. It uses gdupe's existing `objects` rows and writes the same version-3 `gdupe::Fingerprint` fields that gdupe writes today. Existing fingerprints are immediately reusable. Missing or invalidated fingerprints become background work.

The daemon appends only its own operational tables to that same SQLite file:

- `gfingerd_failures` — bounded per-file retry/terminal failure state.
- `gfingerd_deferred_gifs` — exact B2 file IDs temporarily skipped for the known malformed-GIF geometry case.
- `gfingerd_metadata` — daemon bookkeeping such as the last successful B2 inventory scan.

Those tables do not replace or duplicate gdupe's media inventory or fingerprint columns.

## Fingerprint compatibility

`gfingerd` compiles the current gdupe fingerprint and decoder sources unchanged. The compatibility profile is deliberately fixed to the current gdupe values:

- fingerprint version: `3`
- video sample frames: `48`
- GIF sample frames: `32`

The decoder stack therefore remains the same as gdupe currently uses: libjpeg-turbo for JPEG, Windows Imaging Component for PNG/GIF, statically linked libwebp, native MP4/WebM demux, and NVIDIA-driver NVDEC for video. There is no FFmpeg, Media Foundation, Qt, FLTK, Electron, or .NET UI dependency.

## Runtime behavior

The normal loop is:

1. List the canonical B2 media prefix using the dedicated read-only B2 login.
2. Reconcile that live inventory into gdupe's existing SQLite `objects` table.
3. Select supported rows whose fingerprint is missing.
4. Keep configured persistent B2 download connections open and prefetch into a bounded local queue.
5. Fingerprint prefetched media with the unchanged gdupe implementation.
6. Re-read the exact B2 object identity before settlement.
7. Save the fingerprint only if the key and exact B2 file ID are still current.
8. Delete the staged media and continue.

This makes concurrent acquisition safe: a replaced B2 object cannot receive a stale fingerprint because gdupe's existing database write is constrained by both object key and exact file ID.

At cold boot, JPEG/PNG/WebP/GIF work can proceed before the NVIDIA driver is ready. MP4/M4V/WebM work remains pending until NVDEC is available and does not consume its failure budget merely because the driver has not initialized yet.

## Deferred malformed GIFs

The known WIC error `GIF frame rectangle is outside its logical canvas` is temporarily swept into a recoverable queue rather than retried forever. `gfingerd`:

- verifies the exact current B2 identity;
- preserves the downloaded GIF under `%LOCALAPPDATA%\GParty\deferred-gifs\`;
- writes a JSON identity/reason note beside it;
- records that exact file ID in `gfingerd_deferred_gifs` inside the normal gdupe database; and
- excludes only that exact version from normal pending work.

If B2 replaces the object with a new file ID, the new version is eligible immediately. See `TODO.md` for the planned proper normalization/reprocessing work.

## GUI and boot startup

Double-click `gfingerd.exe` to open its native Win32 control panel. The GUI configures the dedicated B2 login, bucket/prefix, inventory interval, fingerprint worker count, persistent download connection count, bounded prefetch capacity, and boot startup. It also shows live queue, throughput, worker, backlog, failure, and deferred-GIF status.

**Save & Start** installs an elevated copy at `%ProgramFiles%\GParty\gfingerd.exe` and creates an `ONSTART` Task Scheduler entry running as Windows SYSTEM at highest privileges. It starts before interactive Windows sign-in and does not require a PIN. The machine copy receives an ACL-restricted configuration and machine-scoped DPAPI-protected B2 credentials under `%ProgramData%\GParty`.

The normal user configuration is stored at:

```text
%LOCALAPPDATA%\GParty\gfingerd.json
```

Staging and logs live under `%LOCALAPPDATA%\GParty`. The fingerprint database itself remains the existing gdupe database.

## Advanced commands

```text
gfingerd.exe --once
gfingerd.exe --status
gfingerd.exe --viewer
gfingerd.exe --set-credentials
```

`--boot-worker`, `--install-boot`, and `--uninstall-boot` are internal startup-management entry points used by the GUI/elevated installer.

## Building

`gfingerd` has its own CMake/vcpkg build and GitHub Actions workflow. It does not alter or package gdupe. The build consumes gdupe's existing database, fingerprint, and decoder source files as shared source until those common primitives are moved to a neutral library in a later refactor.

```powershell
cmake -S apps/gfingerd -B build/gfingerd -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-crt `
  -DVCPKG_OVERLAY_TRIPLETS="$PWD\apps\gfingerd\cmake\triplets"
cmake --build build/gfingerd --config Release
ctest --test-dir build/gfingerd -C Release --output-on-failure
```

The package is intentionally a native static-runtime Windows application with zero shipped DLLs. NVIDIA `nvcuda.dll`/`nvcuvid.dll` are resolved from the installed display driver at runtime rather than redistributed.

This software is based in part on the work of the Independent JPEG Group.
