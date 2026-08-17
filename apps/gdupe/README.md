# gdupe

gdupe is the native Windows duplicate manager for GParty's canonical Backblaze B2 media library. It maintains a durable local inventory, reuses fingerprints for unchanged objects, automatically removes byte-identical duplicates, and presents conservative perceptual candidates for review.

The visible workflow is intentionally small: open, synchronize, analyze, review, and finish. There is no scan button, sensitivity slider, database screen, or confirmation step attached to delete and exclude actions.

## Distribution

gdupe has one Windows distribution format: extract the release ZIP and run `gdupe.exe`.

The application binary, redistributable third-party libraries, and Microsoft C/C++ runtime are built with static linkage. The package contains no DLLs and does not require the Visual C++ Redistributable installer. Windows system DLLs are imported normally for the GUI, shell/COM, networking, CNG hashing, Windows Imaging Component, and Windows Credential Manager.

Moving-video fingerprinting and video preview require an NVIDIA GPU with a driver that exposes CUDA and NVDEC through `nvcuda.dll` and `nvcuvid.dll`. Those DLLs are part of the installed NVIDIA driver. They are loaded at runtime and are never copied into the gdupe package. Still-image and GIF analysis/preview do not depend on NVDEC.

The release package contains `gdupe.exe`, this README, the project license, `config/`, and complete third-party legal material under `licenses/`. Qt, OpenCV, FFmpeg, FLTK, AOSP libavc/libhevc, dav1d, libvpx, and OpenSSL are absent from the redistributable dependency graph.

GitHub Actions publishes `gdupe-windows-x64` with the installed package contents directly as the artifact payload. Downloading the artifact therefore produces one ZIP layer only, with `gdupe.exe` at the archive root.

## First launch and B2 credentials

On first launch, if no B2 credentials are already available, gdupe opens a small native setup window asking for:

- Backblaze B2 Key ID
- Backblaze B2 Application Key

The application key field is password-masked. gdupe uses the entered credentials to initialize the normal B2-backed engine first. Only after that succeeds are the credentials saved as a Windows Generic Credential under `gdupe/backblaze-b2`. A mistyped or rejected key is therefore not persisted.

After the first successful launch, ordinary use is simply to run `gdupe.exe`; the saved B2 login is read from Windows Credential Manager.

For development, automation, or temporary overrides, the process environment is still supported. If both variables are present they take precedence over Credential Manager:

- `B2_KEY_ID`
- `B2_APPLICATION_KEY`

The two variables must be supplied together.

The B2 application key needs `listFiles`, `readFiles`, `writeFiles`, and `deleteFiles`. It also needs `listBuckets` unless it is restricted directly to the configured bucket.

## Configuration

Secrets do not belong in the JSON configuration.

The release includes `config/gdupe.example.json`. If no `config/gdupe.json` exists beside the application, gdupe uses that example file as its default non-secret configuration. Copy it to `config/gdupe.json` only when local settings need to be changed.

The durable database and transient preview/fingerprint cache live under `%LOCALAPPDATA%/gdupe/` by default. Downloaded objects are isolated in gdupe's own `objects-v1` cache subdirectory; cleanup never sweeps unrelated files from the configured cache root. Set `storage.keep_media_cache` to `true` only when local disk space is intentionally available for the canonical media set.

An alternate configuration may be selected with:

```powershell
gdupe.exe --config C:\path\to\gdupe.json
```

## Safety and consistency

B2's live `gallery/` object listing is the source of truth. gdupe maintains a verified canonical inventory document at `_internal/gdupe/canonical-index-v1.json`; this is separate from the randomized R2 generation index used by the web application.

Every destructive batch follows a durable protocol: confirm selected B2 file IDs, journal the intended exact-version deletions locally, delete only those versions, verify the post-delete inventory, write and read-verify the canonical index, reconcile SQLite, and retire completed journal records.

If the process stops during that sequence, the next launch replays the recovery journal before analysis. gdupe does not unlock the review interface while B2, the canonical index, and the local inventory are knowingly inconsistent.

Because acquisition may continue during a long first fingerprint or comparison pass, gdupe performs bounded final synchronization around analysis. Newly arrived objects are fingerprinted, exact-cleaned, and included in a rebuilt queue before review opens. If repeated B2 changes prevent convergence, the app remains safely paused instead of presenting a stale queue or retrying forever.

Exact SHA-256 groups are the only automatic deletion class. Perceptual image, crop/reframe, animated GIF, video re-encode, and strongly evidenced excerpt relationships enter manual review unless **Process all** is invoked.

**Keep both** is a durable pair-level exclusion. It suppresses only that comparison. If either key later points to a different B2 file ID, the old exclusion is discarded rather than silently applying to changed content.

## Media and dependency boundary

Supported canonical media extensions are JPEG, PNG, WebP, GIF, MP4, M4V, and WebM.

| Function | Implementation |
|---|---|
| Windowing, controls, and event loop | Win32 |
| UI rendering and text | Direct2D and DirectWrite |
| B2 credential storage | Windows Credential Manager |
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

The interface uses a per-monitor-DPI-aware Win32 window, native keyboard/focus-accessible controls, Direct2D surfaces, and DirectWrite text. Worker results return to the UI thread through private window messages or a synchronized preview mailbox; decoder workers never mutate HWND or Direct2D state.

MP4/M4V and WebM are demuxed by small source-level container libraries. Compressed packets are handed to NVIDIA NVDEC. The application dynamically resolves the CUDA/NVDEC entry points it uses from the installed display driver; the CUDA Toolkit and NVIDIA Video Codec SDK are not runtime dependencies and are not bundled.

NVDEC capability checks occur after stream metadata reveals codec, chroma format, bit depth, and coded dimensions. Unsupported hardware/profile combinations fail explicitly rather than falling back to an untracked software decoder.

The fingerprint path consumes grayscale luma. Eight-bit luma is copied directly; high-bit-depth surfaces such as HEVC Main 10 are deterministically normalized to 8-bit grayscale before entering the fingerprint pipeline. Video preview uses the same decoder stack with native BGRA output for Direct2D review panes.

The permanent NVIDIA media regression suite contains concrete fixtures for H.264/AVC, HEVC Main, HEVC Main 10, VP8, VP9, and AV1. GPU decode tests skip explicitly on hosted build agents without an NVIDIA runtime/device. The standalone `gdupe-nvdec-selftest-windows-x64` artifact carries the compiled tests and frozen fixtures for real-hardware validation.

After the grayscale boundary, gdupe owns the fingerprint pipeline directly: grayscale resize, low-frequency DCT, compact pHash, 256-bit perceptual hash, crop fingerprints, frame sampling, and timeline aggregation.

## Build

The supported build is 64-bit Windows with CMake 3.28 or newer and vcpkg manifest mode. The vcpkg baseline and source-fetched libraries are pinned. The project uses the `x64-windows-static-crt` triplet so redistributable dependency libraries and the MSVC CRT are static.

No CUDA Toolkit installation and no NVIDIA SDK checkout are required to compile gdupe. A normal build is:

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

`.github/workflows/gdupe-build.yml` performs the clean Windows build, validates the exact dependency closure, verifies `/MT` provenance, runs CPU tests and any available GPU tests, builds the standalone NVIDIA hardware self-test artifact, verifies the release package contains zero DLLs, checks that `gdupe.exe` has no dynamic MSVC/UCRT or redistributable third-party imports, audits the legal bundle, and uploads flat artifact payloads for the application and NVIDIA self-test.

## Licensing and third-party notices

gdupe itself is distributed under the repository's PolyForm Noncommercial License 1.0.0. Third-party components retain their own licenses and other legal terms. The release package keeps complete redistributed copyright, license, notice, and component-specific patent-grant material under `licenses/<component>/`; a URL is not used as a substitute for required local text.

The bundled third-party legal set covers curl and its transitive zlib dependency, libjpeg-turbo and the Independent JPEG Group material it incorporates, libwebp, minimp4, libwebm, SQLite, nlohmann/json, and the MIT `nv-codec-headers` declarations used to call the NVIDIA driver. The libwebm patent-grant document is carried beside its license, and libwebp's vcpkg legal roll-up includes its upstream license and patent material.

`nvcuda.dll` and `nvcuvid.dll` are NVIDIA driver components already installed on the host machine. They are not redistributed by gdupe.

### Required acknowledgement

This software is based in part on the work of the Independent JPEG Group.

### Codec patent scope

The copyright licenses and notices above do not by themselves establish clearance of every standards-essential patent that may apply to a video codec. In particular, use or distribution of software that processes H.264/AVC or H.265/HEVC may involve separate patent-licensing considerations depending on the product, distribution model, volume, and jurisdiction. gdupe does not claim that an API-header license or NVIDIA driver availability substitutes for any separately applicable standards-essential patent license.
