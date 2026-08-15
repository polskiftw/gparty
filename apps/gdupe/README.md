# gdupe

gdupe is the native Windows duplicate manager for GParty's canonical Backblaze B2 media library. It maintains a durable local inventory, reuses fingerprints for unchanged objects, automatically removes byte-identical duplicates, and presents conservative perceptual candidates for review.

The visible workflow is deliberately small: open, wait for synchronization and analysis, review, and finish. There is no scan button, sensitivity slider, database screen, or confirmation step attached to delete and exclude actions.

## Distribution

gdupe has one Windows distribution format: extract the release ZIP and run `gdupe.exe`.

The application binary, redistributable third-party libraries, and Microsoft C/C++ runtime are built with static linkage. The package contains no DLLs and does not require the Visual C++ Redistributable installer. Windows system DLLs are imported normally for the GUI, shell/COM, networking, CNG hashing, and Windows Imaging Component.

Moving-video fingerprinting and video preview intentionally require an NVIDIA GPU with a driver that exposes CUDA and NVDEC through `nvcuda.dll` and `nvcuvid.dll`. Those DLLs are part of the installed NVIDIA driver. They are loaded at runtime and are never copied into the gdupe package. Still-image and GIF analysis/preview do not depend on NVDEC.

The release package is intentionally small at the top level:

- `gdupe.exe` — the application
- `README.md` — this document
- `LICENSE` — gdupe's project license
- `config/` — example configuration
- `licenses/` — complete redistributed third-party legal material

Qt, OpenCV, FFmpeg, FLTK, AOSP libavc/libhevc, dav1d, libvpx, and OpenSSL are absent from the redistributable dependency graph.

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
| Windowing, controls, and event loop | Win32 |
| UI rendering and text | Direct2D and DirectWrite |
| Animated GIF decode, composition, and preview | Windows Imaging Component (WIC) |
| JPEG decode | libjpeg-turbo |
| PNG decode | Windows Imaging Component (WIC) |
| WebP decode | libwebp decoder |
| MP4/M4V demux | source-pinned minimp4 |
| WebM demux | libwebm |
| H.264/AVC decode | NVIDIA NVDEC |
| H.265/HEVC Main and Main 10 decode | NVIDIA NVDEC |
| VP8/VP9 decode | NVIDIA NVDEC |
| AV1 decode | NVIDIA NVDEC |
| NVIDIA API declarations | source-pinned MIT `nv-codec-headers` |
| Backblaze B2 HTTPS | curl using Windows SSPI/Schannel |
| Inventory and recovery journal | SQLite |
| Configuration and index JSON | nlohmann/json |
| Video preview | NVIDIA NVDEC + native BGRA conversion + Direct2D |

The interface uses a per-monitor-DPI-aware Win32 window, native keyboard/focus-accessible buttons, Direct2D surfaces, and DirectWrite text. Worker results return to the UI thread through private window messages or a synchronized preview mailbox; decoder workers never mutate HWND or Direct2D state. Static images, correctly composed animated GIF frames, and decoded video frames are all rendered by the same Direct2D review panes.

Animated GIFs use one shared WIC decoder/compositor for analysis and preview. It reads frame timing, offsets, disposal metadata, logical background state, and premultiplied color pixels. The analysis consumer retains only representative grayscale frames; the preview consumer animates the composed color frames using the native UI timer.

The still-image decoder choices were measured on the same full-decode-to-RGB workload on a Windows Server 2025 x64 Release runner with file I/O excluded. WIC PNG was pixel-identical and about 15% faster than libpng, so PNG uses WIC. WIC JPEG was pixel-identical but 24–28% slower than libjpeg-turbo, so JPEG intentionally retains libjpeg-turbo. WebP remains on libwebp so the application does not depend on an optional Windows codec extension.

### NVIDIA video path

MP4/M4V and WebM remain demuxed by small source-level container libraries. Only compressed video packets are handed to the NVIDIA parser/decoder. gdupe dynamically resolves the small set of CUDA/NVDEC entry points it uses from the installed display driver; the CUDA Toolkit and NVIDIA Video Codec SDK are not runtime dependencies and are not bundled.

The build pins the MIT-licensed `FFmpeg/nv-codec-headers` repository solely for NVIDIA API type and constant declarations. gdupe does not vendor FFmpeg and does not link any FFmpeg library.

Each stream is capability-checked with `cuvidGetDecoderCaps` after its sequence header reveals codec, chroma format, bit depth, and coded dimensions. Unsupported hardware/profile combinations fail explicitly rather than falling back to an untracked software decoder.

NVDEC produces GPU-resident YUV surfaces. The fingerprint path copies only the luma plane. Eight-bit luma is copied directly; high-bit-depth surfaces such as HEVC Main 10 are deterministically normalized to 8-bit grayscale before entering the fingerprint pipeline. That analysis contract is unchanged by the preview implementation.

Video preview uses a separate output mode on the same NVDEC wrapper. Preview decode is bounded to at most 1920×1080, copies the required 4:2:0 luma/chroma planes while the NVDEC surface is mapped, converts NV12 or P016 to opaque BGRA8 using the stream's range and matrix metadata, unmaps the GPU surface, and only then publishes the frame to the UI. The Direct2D panes aspect-fit that BGRA frame exactly like still images and GIFs. Two review panes own independent stoppable decoder workers, playback is muted because no audio path is opened, timestamps pace autoplay, and end-of-stream restarts the local decode for looping.

The native preview deliberately focuses on the common 4:2:0 NV12/P016 surface formats used by the supported fixtures, including 8-bit and Main 10. Unsupported chroma/output layouts fail only that pane with **Preview unavailable** rather than introducing a software-decoder fallback. HDR transfer-function tone mapping is not implemented; HDR material is converted with its declared range/matrix for a deterministic review image rather than display-referred HDR rendering.

The permanent NVIDIA media regression suite has concrete decode fixtures for H.264/AVC, HEVC Main, HEVC Main 10, VP8, VP9, and AV1. CPU-only CI always exercises preview color conversion and aspect-fit math. GPU decode tests skip explicitly on build agents that have no NVIDIA runtime/device; on NVIDIA hardware the preview suite decodes every fixture to BGRA, emits deterministic frame checksums, verifies Main 10, replacement/clean shutdown, two simultaneous preview workers, and malformed-media failure behavior. CI also publishes a standalone `gdupe-nvdec-selftest-windows-x64` artifact so the same binaries and frozen fixtures can be exercised on real NVIDIA hardware without installing CMake, vcpkg, Visual Studio, CUDA Toolkit, or the NVIDIA Video Codec SDK.

After the grayscale boundary, gdupe owns the fingerprint pipeline directly: grayscale resize, low-frequency DCT, compact pHash, 256-bit perceptual hash, crop fingerprints, frame sampling, and timeline aggregation.

This decoder stack defines the canonical fingerprints for the database. The database is intended to be generated from scratch; compatibility with fingerprints produced by older FFmpeg/OpenCV/AOSP/libvpx/dav1d implementations is not part of the contract.

## Fingerprints and matching

Static media uses SHA-256, a compact DCT perceptual hash, a complementary 256-bit high-resolution DCT hash, and multiple centered/corner crop fingerprints. GIF and video add distributed frame fingerprints, an aggregate signature, technical timing metadata, and sequence-aware comparison that can conservatively recognize re-encodes and substantial excerpts.

Fingerprint acquisition uses four bounded B2 download/decoder workers by default; completed fingerprints are committed independently, so a retry reuses finished work. `fingerprints.worker_threads` can be reduced when a narrower B2 connection footprint is preferred.

Pair-space comparison is native C++ and uses all logical CPU threads by default. Set `matching.worker_threads` to a positive number only to override automatic hardware concurrency.

Overlapping candidates are not treated as an independent list of right-side deletions. Manual actions remove every affected relationship. **Process all** constructs deterministic survivors from the current graph and deletes only direct, evidenced neighbors; a transitive-only object is retained.

## Build

The supported build is 64-bit Windows with CMake 3.28 or newer and vcpkg manifest mode. The vcpkg baseline and source-fetched libraries are pinned. The project uses the `x64-windows-static-crt` triplet so redistributable dependency libraries and the MSVC CRT are static.

No CUDA Toolkit installation and no NVIDIA SDK checkout are required to compile gdupe. CMake fetches the pinned MIT NVIDIA interface headers automatically. A normal build is:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
git -C C:\vcpkg checkout 4f6d4ae8247b2dcae554555a135e52bb449dd524
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics

cmake -S apps/gdupe -B build/gdupe -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-crt `
  -DVCPKG_OVERLAY_TRIPLETS="$PWD\apps\gdupe\cmake\triplets"

cmake --build build/gdupe --config Release
ctest --test-dir build/gdupe -C Release --output-on-failure
cmake --install build/gdupe --config Release --prefix dist/gdupe
```

`.github/workflows/gdupe-build.yml` performs the clean Windows build, validates the exact dependency closure, verifies `/MT` provenance, runs CPU tests and any available GPU tests, builds the standalone NVIDIA hardware self-test artifact, verifies the release package contains zero DLLs, checks that `gdupe.exe` has no dynamic MSVC/UCRT, redistributable third-party, or retired video-preview imports, audits the legal bundle, and uploads the ready-to-run ZIP artifact.

## Licensing and third-party notices

gdupe itself is distributed under the repository's PolyForm Noncommercial License 1.0.0. Third-party components retain their own licenses and other legal terms. The release package keeps complete redistributed copyright, license, notice, and component-specific patent-grant material under `licenses/<component>/`; a URL is not used as a substitute for required local text.

The bundled third-party legal set covers curl and its transitive zlib dependency, libjpeg-turbo and the Independent JPEG Group material it incorporates, libwebp, minimp4, libwebm, SQLite, nlohmann/json, and the MIT `nv-codec-headers` declarations used to call the NVIDIA driver. The libwebm patent-grant document is carried beside its license, and libwebp's vcpkg legal roll-up includes its upstream license and patent material.

`nvcuda.dll` and `nvcuvid.dll` are NVIDIA driver components already installed on the host machine. They are not redistributed by gdupe.

### Required acknowledgements

The acknowledgement text required by dependencies is kept here rather than scattered across separate notice documents:

> This software is based in part on the work of the Independent JPEG Group.

The presence of a third-party component in gdupe does not place gdupe's own source under that component's license except where a specific third-party-derived file says otherwise.

### Codec patent scope

The copyright licenses and notices above do not by themselves establish clearance of every standards-essential patent that may apply to a video codec. In particular, use or distribution of software that processes H.264/AVC or H.265/HEVC may involve separate patent-licensing considerations depending on the product, distribution model, volume, and jurisdiction. gdupe does not claim that an API-header license or NVIDIA driver availability substitutes for any separately applicable standards-essential patent license.
