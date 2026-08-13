# gdupe runtime surface

The portable package is intentionally constrained to one executable and four
FFmpeg DLLs. CI enumerates every packaged `.dll` and fails if this list changes:

- `avformat-63.dll`: local GIF, MOV/MP4/M4V, and Matroska/WebM demuxing
- `avcodec-63.dll`: GIF, H.264, HEVC, VP8, VP9, and AV1 decoding
- `avutil-61.dll`: FFmpeg data, memory, time-base, and error utilities
- `swscale-10.dll`: decoder pixel formats to 8-bit grayscale only

The FFmpeg DLLs come from commit
`6bbc22dc09c214b2f5334afa30167fa1990eb5df`. No command-line programs,
networking, encoders, muxers, filters, devices, audio conversion, or other
FFmpeg shared libraries are built. `FFMPEG-SURFACE.md` records the exact
configure closure, including the VP9 parser and superframe splitter selected by
the VP9 decoder.

Everything below is statically linked into `gdupe.exe` with the static MSVC
runtime:

| Component | Used for | Deliberately disabled or excluded |
|---|---|---|
| FLTK 1.4.5 | Windows, widgets, basic GDI drawing, animated GIF preview | Shared libraries, Forms, FLUID, options tool, examples, tests, docs, OpenGL, printing, filesystem helpers, SVG, GDI+, Cairo |
| libjpeg-turbo | JPEG decode | Encoding and command-line tools are not used by gdupe |
| libpng + zlib | PNG decode | PNG encoding is not used by gdupe |
| libwebp decoder | WebP decode | WebP encoding, muxing, animation decoding, and tools |
| curl HTTPS | Backblaze B2 requests | Default non-HTTP protocol feature bundle |
| SQLite | Durable inventory and recovery journal | JSON extension, SQLite shell, and dynamic library |
| nlohmann JSON | Configuration and B2/index documents | No runtime component |

Windows system DLLs provide the normal GUI, COM, shell, networking, CNG
SHA-1/SHA-256, and Media Foundation MFPlay video-preview APIs. They are part of
Windows, not app-local files. Video preview is separate from fingerprint
decoding so it cannot expand the custom FFmpeg capability surface. OpenSSL is
not in the dependency graph.

Qt and OpenCV are absent from the source dependency graph, link graph, package,
and import table. Package audit also rejects the dynamic MSVC runtime and any
plugin or tools directory.
