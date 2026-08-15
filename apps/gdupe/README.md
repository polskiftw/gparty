# gdupe

gdupe is the native Windows duplicate manager for GParty's canonical Backblaze B2 media library. It maintains a durable local inventory, reuses fingerprints for unchanged objects, automatically removes byte-identical duplicates, and presents conservative perceptual candidates for review.

The visible workflow is deliberately small: open, wait for synchronization and analysis, review, and finish. There is no scan button, sensitivity slider, database screen, or confirmation step attached to delete and exclude actions.

## Distribution

gdupe has one Windows distribution format: extract the release ZIP and run `gdupe.exe`.

The application binary, third-party libraries, and Microsoft C/C++ runtime are built with static linkage. The package contains no third-party DLLs and does not require the Visual C++ Redistributable installer. Windows system DLLs are imported normally for the GUI, shell/COM, networking, CNG hashing, and Media Foundation preview; those are operating-system components rather than app-local dependencies.

The release package is intentionally small at the top level:

- `gdupe.exe` — the application
- `README.md` — this document
- `LICENSE` — gdupe's project license
- `config/` — example configuration
- `licenses/` — complete third-party legal material and required source/notice records

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

Because acquisition may continue during a long first fingerprint or comparison pass, gdupe performs bounded final synchronization around analysis. Newly arrived objects are fingerprinted, exact-cleaned, and included in a rebuilt queue before review opens. If repeated B2 changes prevent convergence, the app remains safely paused instead of presenting a stale queue or retrying forever.

Exact SHA-256 groups are the only automatic deletion class. The survivor is deterministic and quality-aware. Perceptual image, crop/reframe, animated GIF, video re-encode, and strongly evidenced excerpt relationships enter manual review unless **Process all** is invoked.

**Keep both** is a durable pair-level exclusion. It suppresses only that comparison, preserving useful matching between either object and other media. If either key later points to a different B2 file ID, the old exclusion is discarded rather than silently applying to changed content.

## Configuration

Copy `config/gdupe.example.json` to `config/gdupe.json` beside `gdupe.exe` and edit non-secret settings if necessary. Ordinary use should not require matcher changes.

Credentials are read only from the process environment:

- `B2_KEY_ID`
- `B2_APPLICATION_KEY`

The B2 application key needs `listFiles`, `readFiles`, `writeFiles`, and `deleteFiles`. It also needs `listBuckets` unless it is restricted directly to the configured bucket.

The durable database and transient preview/fingerprint cache live under `%LOCALAPPDATA%/gdupe/` by default. Downloaded objects are isolated in gdupe's own `objects-v1` cache subdirectory; cleanup never sweeps unrelated files from the configured cache root. Set `storage.keep_media_cache` to `true` only when local disk space is intentionally available for the canonical media set.

Run with an alternate configuration using:

```powershell
gdupe.exe --config C:\path\to\gdupe.json
```

## Media and dependency boundary

Supported canonical media extensions are JPEG, PNG, WebP, GIF, MP4, M4V, and WebM.

| Function | Implementation |
|---|---|
| Windows UI and composed GIF frames | source-pinned upstream FLTK 1.4.5 |
| JPEG decode | libjpeg-turbo |
| PNG decode | libpng + zlib |
| WebP decode | libwebp decoder |
| MP4/M4V demux | source-pinned minimp4 |
| H.264/AVC decode | source-pinned AOSP libavc decoder |
| H.265/HEVC decode | source-pinned AOSP libhevc decoder |
| WebM demux | libwebm |
| VP8/VP9 decode | libvpx |
| AV1 decode | dav1d |
| Backblaze B2 HTTPS | curl using Windows SSPI/Schannel |
| Inventory and recovery journal | SQLite |
| Configuration and index JSON | nlohmann/json |
| Video preview | Windows Media Foundation MFPlay |

FLTK is fetched at a fixed upstream commit and configured only through its supported build options. gdupe does not patch FLTK source or rewrite FLTK target source lists. Forms compatibility, FLUID, fltk-options, examples, tests, documentation, OpenGL, the SVG feature option, GDI+, Cairo, and shared-library output are disabled through the upstream configuration surface. FLTK's upstream `fltk_images` target still compiles its bundled NanoSVG helper source in this configuration, so gdupe carries the NanoSVG notice under `licenses/fltk/` as part of the legal bundle.

The AOSP H.264 and HEVC libraries do not provide the Windows/MSVC build used by gdupe. Their decoder code is pinned upstream, while narrowly scoped Win32 threading/compiler adaptations are applied for the gdupe build. Those changed files are explicitly marked, and the release package carries each decoder's Apache license, upstream `NOTICE`, and a `SOURCE.txt` record identifying the exact upstream commit and local adaptations.

Video decoders expose planar YUV frames. gdupe consumes the luma/Y plane directly: 8-bit luma is copied as-is and higher bit depths are deterministically mapped to 8-bit. No general pixel-conversion framework is required. Animated GIF and static RGB image paths use the same integer BT.601 grayscale boundary.

After that boundary, gdupe owns the fingerprint pipeline directly: grayscale resize, low-frequency DCT, compact pHash, 256-bit perceptual hash, crop fingerprints, frame sampling, and timeline aggregation.

This decoder stack defines the canonical fingerprints for the database. The database is intended to be generated from scratch; compatibility with fingerprints produced by older FFmpeg/OpenCV/CLI implementations is not part of the contract.

## Fingerprints and matching

Static media uses SHA-256, a compact DCT perceptual hash, a complementary 256-bit high-resolution DCT hash, and multiple centered/corner crop fingerprints. GIF and video add distributed frame fingerprints, an aggregate signature, technical timing metadata, and sequence-aware comparison that can conservatively recognize re-encodes and substantial excerpts.

Fingerprint acquisition uses four bounded B2 download/decoder workers by default; completed fingerprints are committed independently, so a retry reuses finished work. `fingerprints.worker_threads` can be reduced when a narrower B2 connection footprint is preferred.

Pair-space comparison is native C++ and uses all logical CPU threads by default. Set `matching.worker_threads` to a positive number only to override automatic hardware concurrency.

Overlapping candidates are not treated as an independent list of right-side deletions. Manual actions remove every affected relationship. **Process all** constructs deterministic survivors from the current graph and deletes only direct, evidenced neighbors; a transitive-only object is retained.

## Build

The supported build is 64-bit Windows with CMake 3.28 or newer and vcpkg manifest mode. The vcpkg baseline and source-fetched libraries are pinned. The project uses the `x64-windows-static-crt` triplet so dependency libraries and the MSVC CRT are static.

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
  -DVCPKG_OVERLAY_TRIPLETS="$PWD\apps\gdupe\cmake\triplets" `
  -DGDUPE_LIBAVC_DIR=C:\path\to\libavc-sdk `
  -DGDUPE_LIBHEVC_DIR=C:\path\to\libhevc-sdk

cmake --build build/gdupe --config Release
ctest --test-dir build/gdupe -C Release --output-on-failure
cmake --install build/gdupe --config Release --prefix dist/gdupe
```

`.github/workflows/gdupe-build.yml` performs the clean Windows build, validates the static dependency closure, runs the tests, verifies the release package contains zero DLLs, checks that `gdupe.exe` has no dynamic MSVC/UCRT or third-party imports, audits the legal bundle, and uploads the ready-to-run ZIP artifact.

## Licensing and third-party notices

gdupe itself is distributed under the repository's PolyForm Noncommercial License 1.0.0. Third-party components retain their own licenses and other legal terms. The release package keeps complete redistributed copyright, license, notice, and component-specific patent-grant material under `licenses/<component>/`; a URL is not used as a substitute for a required local text.

The bundled third-party legal set covers FLTK and its bundled NanoSVG helper, curl, dav1d, libjpeg-turbo and the Independent JPEG Group material it incorporates, libpng, zlib, libwebp, minimp4, AOSP libavc, AOSP libhevc, libwebm, libvpx, SQLite, and nlohmann/json. AOSP `NOTICE` files are carried with the two modified decoder builds. The WebM patent-grant documents for libvpx and libwebm are carried beside their licenses; libvpx also carries the ISC notice for its x86inc assembly helper, and libwebp's vcpkg legal roll-up includes its upstream license and patent material.

### Required acknowledgements

The acknowledgement text required by dependencies is kept here rather than scattered across separate notice documents:

> gdupe is based in part on the work of the FLTK project (https://www.fltk.org).

> This software is based in part on the work of the Independent JPEG Group.

The presence of a third-party component in gdupe does not place gdupe's own source under that component's license except where a specific third-party-derived file says otherwise. The AOSP Windows adaptation files under `third_party/aosp/libavc/` and `third_party/aosp/libhevc/` carry their upstream Apache licensing and modification notices directly.

### Codec patent scope

The copyright licenses and notices above do not by themselves establish clearance of every standards-essential patent that may apply to a video codec. In particular, distribution of software implementing H.264/AVC or H.265/HEVC may involve separate patent-licensing considerations depending on the product, distribution model, volume, and jurisdiction. gdupe does not claim that its bundled open-source codec licenses substitute for any separately applicable standards-essential patent license.
