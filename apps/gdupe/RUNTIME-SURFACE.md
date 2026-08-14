# gdupe runtime surface

The portable distribution is intentionally a **single application executable**.
No third-party DLLs, redistributable installers, plugin trees, or helper programs
are shipped. CI fails if any `.dll` appears anywhere in the package or if
`gdupe.exe` directly imports a DLL that is not supplied by Windows itself.

All redistributable dependencies below are statically linked into `gdupe.exe`
with the static MSVC runtime (`/MT`):

| Component | Used for | Deliberately constrained |
|---|---|---|
| FLTK 1.4.5 | Windows UI and composed animated-GIF frames | Shared libraries, Forms, FLUID, options tool, examples, tests, docs, OpenGL, printing, filesystem helpers, SVG, GDI+, Cairo |
| libjpeg-turbo | JPEG decode | Encoding and command-line tools are not used |
| libpng + zlib | PNG decode | PNG encoding is not used |
| libwebp decoder | WebP decode | Encoding, muxing, animation decode, and tools are not used |
| minimp4 | MP4/M4V demux and AVC/HEVC sample extraction | Header-only demux path; no media framework or networking |
| AOSP libavc | H.264/AVC decode | Decoder library only; Windows/MSVC adaptation is limited to compiler/thread portability glue |
| AOSP libhevc | H.265/HEVC decode | Decoder library only; Windows/MSVC adaptation is limited to compiler/thread portability glue |
| libwebm | WebM demux | Parser path only |
| libvpx | VP8/VP9 decode | Decoder use only; high-bit-depth support retained |
| dav1d | AV1 decode | Decoder use only; film grain is disabled for deterministic fingerprint luma |
| curl HTTPS | Backblaze B2 requests | Default non-HTTP protocol bundle is excluded |
| SQLite | Durable inventory and recovery journal | SQLite shell and dynamic library are absent |
| nlohmann JSON | Configuration and B2/index documents | Header-only; no runtime component |

## Fingerprint decode surface

Static images use the dedicated JPEG, PNG, and WebP decoders already linked into
gdupe. Animated GIFs use FLTK's composed animation frames. MP4/M4V is demuxed by
minimp4 and decoded by AOSP libavc or libhevc. WebM is demuxed by libwebm and
decoded by libvpx or dav1d.

Video codecs expose planar YUV output. Fingerprinting consumes the luma/Y plane
directly instead of converting through a general pixel-format library. 8-bit
luma is copied directly; higher bit depths are deterministically scaled to
8-bit. The resulting grayscale frames feed gdupe's own resize, DCT, pHash,
256-bit perceptual hash, crop-hash, and timeline logic.

The fingerprint database is built from this decoder stack from scratch. It does
not attempt byte-for-byte compatibility with the retired FFmpeg implementation.
The static media path therefore defines the canonical fingerprint behavior.

## Windows-provided runtime surface

Windows system DLLs provide the normal GUI, COM, shell, networking, CNG hashing,
and Media Foundation MFPlay preview APIs. These are part of Windows and are not
app-local dependencies. Media Foundation is used for user-facing video preview;
it is separate from fingerprint decoding.

Qt, OpenCV, FFmpeg, OpenSSL, and the dynamic MSVC/UCRT runtimes are absent from
the redistributable dependency graph. Package audit also rejects any unexpected
plugin or tools directory.
