# gdupe

gdupe is the native Windows duplicate manager for GParty's canonical Backblaze B2 media library. It synchronizes a durable local inventory, reuses fingerprints for unchanged objects, automatically removes byte-identical copies, and presents only conservative perceptual candidates for review.

The visible workflow is deliberately small: open, wait for synchronization and analysis, review, and finish. There is no scan button, sensitivity slider, database screen, or confirmation step attached to delete and exclude actions.

## Safety and consistency

B2's live `gallery/` object listing is the source of truth. gdupe maintains a verified canonical inventory document at `_internal/gdupe/canonical-index-v1.json`; this is separate from the randomized R2 generation index used by the web application.

Every destructive batch follows the same durable protocol:

1. Confirm each selected key still resolves to the exact B2 file ID analyzed by gdupe.
2. Commit the intended exact-version deletions to the local SQLite recovery journal.
3. Delete only those B2 versions and verify each acknowledgement.
4. Obtain a stable post-delete B2 inventory.
5. write and read-verify the canonical B2 index, repeating if acquisition changed the inventory during publication.
6. Reconcile SQLite and retire the journal records.

If the process stops between those steps, the next launch replays the journal before analysis. gdupe does not unlock the review interface while B2, the canonical index, and the durable local inventory are knowingly inconsistent.

Exact SHA-256 groups are the only automatic deletion class. The survivor is deterministic and quality-aware. Perceptual image, crop/reframe, animated GIF, video re-encode, and strongly evidenced excerpt relationships always enter manual review unless **Process all** is invoked.

**Keep both** is a durable pair-level exclusion. It suppresses only that comparison, preserving useful matching between either object and other media.

## Configuration

Copy `config/gdupe.example.json` to `config/gdupe.json` beside the installed executable and edit non-secret settings if necessary. Ordinary use should not require matcher changes.

Credentials are read only from the process environment:

- `B2_KEY_ID`
- `B2_APPLICATION_KEY`

The B2 application key needs `listFiles`, `readFiles`, `writeFiles`, and `deleteFiles`. It also needs `listBuckets` unless it is restricted directly to the configured bucket.

The default durable database and transient preview/fingerprint cache live under `%LOCALAPPDATA%/gdupe/`. Set `storage.keep_media_cache` to `true` only when local disk space is intentionally available for the canonical media set.

The supported canonical media extensions are JPEG, PNG, WebP, GIF, MP4, M4V, and WebM. The decoder also accepts BMP, TIFF, MOV, and MKV if they appear later.

Run with an alternate configuration using:

```powershell
gdupe.exe --config C:\path\to\gdupe.json
```

## Fingerprints and matching

Static media uses SHA-256, a DCT perceptual hash, a complementary 256-bit gradient hash, and multiple centered/corner crop fingerprints. GIF and video add evenly distributed frame fingerprints, an aggregate signature, technical timing metadata, and sequence-aware comparison that can conservatively recognize re-encodes and substantial excerpts.

Pair-space comparison is native C++ and uses all logical CPU threads by default. Set `matching.worker_threads` to a positive number only to override automatic hardware concurrency.

Overlapping candidates are not treated as an independent list of right-side deletions. Manual actions remove every affected relationship. **Process all** constructs deterministic survivors from the current graph and deletes only direct, evidenced neighbors; a transitive-only object is retained.

## Build

The supported build is 64-bit Windows with CMake 3.28 or newer and vcpkg manifest mode. Dependencies are pinned by the vcpkg baseline.

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
git -C C:\vcpkg checkout 4f6d4ae8247b2dcae554555a135e52bb449dd524
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake -S apps/gdupe -B build/gdupe -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build/gdupe
ctest --test-dir build/gdupe --output-on-failure
cmake --install build/gdupe --prefix dist/gdupe
```

`.github/workflows/gdupe-build.yml` performs the same clean Windows build, runs the critical core tests, deploys the Qt runtime, and uploads a ready-to-run ZIP artifact.

