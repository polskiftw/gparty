# gdupe

gdupe is the native Windows duplicate manager for GParty's canonical Backblaze B2 media library. It synchronizes a durable local inventory, reuses fingerprints for unchanged objects, automatically removes byte-identical copies, and presents only conservative perceptual candidates for review.

The visible workflow is deliberately small: open, wait for synchronization and analysis, review, and finish. There is no scan button, sensitivity slider, database screen, or confirmation step attached to delete and exclude actions.

## Portable Windows package

The release package contains one application binary: `gdupe.exe`. Every redistributable library and the Microsoft C/C++ runtime are statically linked with `/MT`; no third-party DLLs or Visual C++ Redistributable installer are shipped. Required upstream license notices remain under `licenses/`. Extract the ZIP and run `gdupe.exe` directly.

Windows system DLLs are still imported normally for the GUI, shell/COM, networking, CNG hashing, and Media Foundation preview. They are part of Windows rather than app-local dependencies. CI rejects any packaged `.dll`, any dynamic MSVC/UCRT import, and any direct DLL import that does not resolve to Windows itself.

Qt, OpenCV, FFmpeg, and OpenSSL are absent from the redistributable dependency graph.

## Safety and consistency

B2's live `gallery/` object listing is the source of truth. gdupe maintains a verified canonical inventory document at `_internal/gdupe/canonical-index-v1.json`; this is separate from the randomized R2 generation index used by the web application.

Every destructive batch follows the same durable protocol:

1. Confirm each selected key still resolves to the exact B2 file ID analyzed by gdupe.
2. Commit the intended exact-version deletions to the local SQLite recovery journal.
3. Delete only those B2 versions and verify each acknowledgement.
4. Obtain a stable post-delete B2 inventory.
5. Write and read-verify the canonical B2 index, repeating if acquisition changed the inventory during publication.
6. Remove superseded versions of the internal index after its current version is proven stable.
7. Reconcile SQLite and retire the journal records.

If the process stops between those steps, the next launch replays the journal before analysis. gdupe does not unlock the review interface while B2, the canonical index, and the durable local inventory are knowingly inconsistent.

Because acquisition may continue while a long first fingerprint or comparison pass is running, gdupe performs bounded final synchronization around analysis. Any newly arrived objects are fingerprinted, exact-cleaned, and included in a rebuilt queue before review opens. If repeated B2 changes prevent convergence, the app remains safely paused instead of presenting a stale queue or retrying forever.

Exact SHA-256 groups are the only automatic deletion class. The survivor is deterministic and quality-aware. Perceptual image, crop/reframe, animated GIF, video re-encode, and strongly evidenced excerpt relationships always enter manual review unless **Process all** is invoked.

**Keep both** is a durable pair-level exclusion. It suppresses only that comparison, preserving useful matching between either object and other media. If either key later points to a different B2 file ID, the old exclusion is discarded rather than silently applying to changed content.

## Configuration

Copy `config/gdupe.example.json` to `config/gdupe.json` beside the installed executable and edit non-secret settings if necessary. Ordinary use should not require matcher changes.

Credentials are read only from the process environment:

- `B2_KEY_ID`
- `B2_APPLICATION_KEY`

The B2 application key needs `listFiles`, `readFiles`, `writeFiles`, and `deleteFiles`. It also needs `listBuckets` unless it is restricted directly to the configured bucket.

The default durable database and transient preview/fingerprint cache live under `%LOCALAPPDATA%/gdupe/`. Downloaded objects are isolated in gdupe's own `objects-v1` cache subdirectory; cleanup never sweeps unrelated files from the configured cache root. Set `storage.keep_media_cache` to `true` only when local disk space is intentionally available for the canonical media set.

## Media decoding

The supported canonical media extensions are JPEG, PNG, WebP, GIF, MP4, M4V, and WebM.

Static-image decoding is deliberately small:

- JPEG: libjpeg-turbo
- PNG: libpng + zlib
- WebP: libwebp decoder

Animated and video fingerprinting uses a fully static format/codec stack:

- GIF: FLTK's composed animated-GIF frames
- MP4/M4V demux: source-pinned minimp4
- H.264/AVC decode: source-pinned AOSP libavc
- H.265/HEVC decode: source-pinned AOSP libhevc
- WebM demux: libwebm
- VP8/VP9 decode: libvpx
- AV1 decode: dav1d

The AOSP H.264 and HEVC libraries do not provide a supported MSVC/Windows target upstream, so gdupe's build applies narrow portability adaptations for Win32 threading, MSVC intrinsics/alignment, and upstream Unix-only build assumptions. Their codec implementation remains pinned upstream code. CI separately proves both decoder libraries build as static `/MT` archives with no decoder DLLs and can decode real conformance bitstreams.

Video decoders expose planar YUV frames. gdupe consumes the luma/Y plane directly: 8-bit luma is copied as-is and higher bit depths are deterministically mapped to 8-bit. No general pixel conversion framework is required. Animated GIF and static RGB image paths use the same integer BT.601 grayscale boundary.

After that boundary, gdupe owns the entire fingerprint pipeline directly: grayscale resize, low-frequency DCT, compact pHash, 256-bit perceptual hash, crop fingerprints, frame sampling, and timeline aggregation.

This decoder stack defines the canonical fingerprints for the database. The database is intended to be generated from scratch; compatibility with fingerprints produced by older FFmpeg/OpenCV/CLI implementations is not part of the contract.

Qt is absent. The window layer is source-pinned FLTK 1.4.5 and statically linked under FLTK's license terms. Forms compatibility, FLUID, fltk-options, examples, tests, documentation, OpenGL, printing, filesystem helpers, SVG, GDI+ drawing, Cairo integration, and shared-library output are disabled. Video preview uses the Windows Media Foundation MFPlay API and is separate from fingerprint decoding.

See [`RUNTIME-SURFACE.md`](RUNTIME-SURFACE.md) for the exact runtime/dependency boundary.

Run with an alternate configuration using:

```powershell
gdupe.exe --config C:\path\to\gdupe.json
```

## Fingerprints and matching

Static media uses SHA-256, a compact DCT perceptual hash, a complementary 256-bit high-resolution DCT hash, and multiple centered/corner crop fingerprints. GIF and video add distributed frame fingerprints, an aggregate signature, technical timing metadata, and sequence-aware comparison that can conservatively recognize re-encodes and substantial excerpts.

Fingerprint acquisition uses four bounded B2 download/decoder workers by default; completed fingerprints are committed independently, so a retry reuses all finished work. `fingerprints.worker_threads` can be reduced when a narrower B2 connection footprint is preferred.

Pair-space comparison is native C++ and uses all logical CPU threads by default. Set `matching.worker_threads` to a positive number only to override automatic hardware concurrency.

Overlapping candidates are not treated as an independent list of right-side deletions. Manual actions remove every affected relationship. **Process all** constructs deterministic survivors from the current graph and deletes only direct, evidenced neighbors; a transitive-only object is retained.

## Build

The supported build is 64-bit Windows with CMake 3.28 or newer and vcpkg manifest mode. The vcpkg baseline and source-fetched libraries are pinned. The project uses the `x64-windows-static-crt` triplet so both dependency libraries and the MSVC CRT are static.

The GitHub workflow additionally builds two source-pinned AOSP decoder SDKs:

- `apps/gdupe/scripts/build_libavc.ps1` → static `libavcdec.lib`
- `apps/gdupe/scripts/build_libhevc.ps1` → static `libhevcdec.lib`

For a manual build, those scripts must first produce SDK directories and `GDUPE_LIBAVC_DIR` / `GDUPE_LIBHEVC_DIR` must point at them. A normal CMake configure then looks like:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
git -C C:\vcpkg checkout 4f6d4ae8247b2dcae554555a135e52bb449dd524
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics

cmake -S apps/gdupe -B build/gdupe -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-crt `
  -DVCPKG_OVERLAY_TRIPLETS="$PWD\apps\gdupe\triplets" `
  -DGDUPE_LIBAVC_DIR=C:\path\to\libavc-sdk `
  -DGDUPE_LIBHEVC_DIR=C:\path\to\libhevc-sdk

cmake --build build/gdupe --config Release
ctest --test-dir build/gdupe -C Release --output-on-failure
cmake --install build/gdupe --config Release --prefix dist/gdupe
```

`.github/workflows/gdupe-build.yml` performs the clean Windows build, validates the exact static dependency closure and trimmed FLTK configuration, runs tests, verifies the portable package contains zero DLLs, checks that `gdupe.exe` has no dynamic MSVC/UCRT or third-party imports, and uploads the ready-to-run ZIP artifact.
