# gdupe

gdupe is the native Windows duplicate manager for GParty's canonical Backblaze B2 media library. It synchronizes a durable local inventory, reuses fingerprints for unchanged objects, automatically removes byte-identical copies, and presents only conservative perceptual candidates for review.

The visible workflow is deliberately small: open, wait for synchronization and analysis, review, and finish. There is no scan button, sensitivity slider, database screen, or confirmation step attached to delete and exclude actions.

## Portable Windows package

The release package contains `gdupe.exe` and exactly four application-owned DLLs: `avcodec-63.dll`, `avformat-63.dll`, `avutil-61.dll`, and `swscale-10.dll`. FLTK, JPEG, PNG, WebP, curl, SQLite, JSON, and the Microsoft C/C++ runtime are statically linked with `/MT`. Windows CNG supplies SHA-1/SHA-256, so OpenSSL is absent too. There is no Qt, OpenCV, plugin tree, Visual C++ Redistributable installer, or app-local `MSVCP`, `VCRUNTIME`, or `CONCRT` DLL. Required upstream license notices are under `licenses/`. Extract the ZIP and run `gdupe.exe` directly.

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

Video and animated-image fingerprinting links directly to four custom FFmpeg DLLs built from pinned upstream commit `6bbc22dc09c214b2f5334afa30167fa1990eb5df`: `avcodec-63.dll`, `avformat-63.dll`, `avutil-61.dll`, and `swscale-10.dll`. There is no `ffmpeg.exe`, `ffprobe.exe`, `avfilter` DLL, temporary PNG frame pipeline, encoder, muxer, filter, zlib, or WinPthreads runtime.

Moving-media input is intentionally limited to the formats present in GParty's canonical extension policy. The enabled demuxers are GIF, MOV/MP4 (which also covers containerized M4V), and Matroska/WebM. The decoder whitelist is GIF, H.264, HEVC, VP8, VP9, and AV1. FFmpeg necessarily selects its VP9 parser and VP9 superframe splitter with the VP9 decoder; CI audits those two transitive components alongside every explicitly requested component and rejects anything else. Network protocols are disabled; the only protocol is local `file` input.

Decoded frames remain in memory. gdupe samples them directly and uses `swscale` only to normalize decoder pixel formats to 8-bit grayscale for its DCT hashes. That is the sole reason `swscale-10.dll` is present. `avutil` supplies FFmpeg's shared data/error utilities, `avcodec` performs decoding, and `avformat` opens and demuxes the local container. See [`FFMPEG-SURFACE.md`](FFMPEG-SURFACE.md) for the complete requirement-to-component audit.

OpenCV is deliberately absent. Its only former jobs were grayscale conversion, resize, and low-frequency DCT, so gdupe now performs those operations directly. JPEG, PNG, and WebP files are decoded by their small source libraries and converted immediately to 8-bit BT.601 grayscale before resize and DCT. FFmpeg converts moving-media frames directly to `GRAY8`. The matching algorithms therefore remain grayscale; RGB exists only at the static decoder boundary and for screen preview.

Qt is also absent. The window layer is source-pinned FLTK 1.4.5, statically linked under FLTK's LGPL static-linking exception. Forms compatibility, FLUID, fltk-options, examples, tests, documentation, OpenGL, printing, filesystem helpers, SVG, GDI+ drawing, Cairo integration, and shared-library output are all disabled. Animated GIF preview uses FLTK's built-in animated GIF image class. Video preview uses the Windows Media Foundation MFPlay API and does not enlarge the packaged FFmpeg build. `licenses/fltk/` identifies the exact source and includes FLTK's license.

The supported canonical media extensions are JPEG, PNG, WebP, GIF, MP4, M4V, and WebM. Fingerprint version 3 records the direct grayscale/DCT implementation so older OpenCV- or CLI-derived fingerprints cannot be mistaken for the new results.

Run with an alternate configuration using:

```powershell
gdupe.exe --config C:\path\to\gdupe.json
```

## Fingerprints and matching

Static media uses SHA-256, a compact DCT perceptual hash, a complementary 256-bit high-resolution DCT hash, and multiple centered/corner crop fingerprints. GIF and video add evenly distributed frame fingerprints, an aggregate signature, technical timing metadata, and sequence-aware comparison that can conservatively recognize re-encodes and substantial excerpts.

Fingerprint acquisition uses four bounded B2 download/decoder workers by default; completed fingerprints are committed independently, so a retry reuses all finished work. `fingerprints.worker_threads` can be reduced when a narrower B2 connection footprint is preferred.

Pair-space comparison is native C++ and uses all logical CPU threads by default. Set `matching.worker_threads` to a positive number only to override automatic hardware concurrency.

Overlapping candidates are not treated as an independent list of right-side deletions. Manual actions remove every affected relationship. **Process all** constructs deterministic survivors from the current graph and deletes only direct, evidenced neighbors; a transitive-only object is retained.

## Build

The supported build is 64-bit Windows with CMake 3.28 or newer and vcpkg manifest mode. Library dependencies are pinned by the vcpkg baseline; FLTK is fetched from exact commit `a9b1113516ffd15fc7602a6d425a317df30f4720`. The GitHub workflow builds the four minimal shared FFmpeg libraries from their pinned source commit with MSVC and `/MT`, audits their explicit capabilities, imports, license mode, and exact DLL surface, and caches that validated SDK for later gdupe builds.

For a manual distributable install, `-DGDUPE_FFMPEG_DIR` must point to a matching minimal SDK containing the exact four DLLs, their four MSVC import libraries under `lib/`, installed headers under `include/`, and the FFmpeg license/source/build-configuration files. CMake deliberately refuses an incomplete SDK instead of globbing arbitrary FFmpeg binaries.

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
git -C C:\vcpkg checkout 4f6d4ae8247b2dcae554555a135e52bb449dd524
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake -S apps/gdupe -B build/gdupe -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-crt `
  -DVCPKG_OVERLAY_TRIPLETS="$PWD\apps\gdupe\triplets" `
  -DGDUPE_FFMPEG_DIR=C:\path\to\minimal-ffmpeg
cmake --build build/gdupe --config Release
ctest --test-dir build/gdupe -C Release --output-on-failure
cmake --install build/gdupe --config Release --prefix dist/gdupe
```

`.github/workflows/gdupe-build.yml` performs the clean Windows build, validates the minimal FFmpeg surface and exact static dependency closure, proves Qt and OpenCV are absent, decodes an embedded H.264/MP4 fixture through the DLL API, exercises the direct PNG/JPEG/WebP decoders, verifies that no packaged binary imports the dynamic MSVC runtime, rejects every DLL except the four approved FFmpeg libraries, and uploads a ready-to-run ZIP artifact. The complete boundary is recorded in [`RUNTIME-SURFACE.md`](RUNTIME-SURFACE.md).
