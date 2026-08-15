# gdupe runtime surface

The portable distribution is intentionally a **single application executable**.
No third-party DLLs, redistributable installers, plugin trees, or helper programs
are shipped. CI fails if any `.dll` appears anywhere in the package or if
`gdupe.exe` directly imports a DLL that is not supplied by Windows itself.

All redistributable dependencies below are statically linked into `gdupe.exe`
with the static MSVC runtime (`/MT`):

| Component | Used for | Deliberately constrained |
|---|---|---|
| FLTK 1.4.5 | Windows UI and composed animated-GIF frames | Source-pinned and source-pruned to gdupe's exact Win32/UI provider closure; image target is GIF-only. No printer/PostScript/PDF path, native/file chooser source, FLTK JPEG/PNG/SVG decoders, alternate built-in theme implementations, shared libraries, Forms, FLUID, options tool, examples, tests, docs, OpenGL, GDI+, or Cairo |
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

## FLTK source boundary

FLTK's upstream CMake feature switches do not all remove source files from the
Windows static-library target. In FLTK 1.4.5 specifically,
`FLTK_OPTION_PRINT_SUPPORT=OFF` only defines `FL_NO_PRINT_SUPPORT` on the X11
path. gdupe therefore does not rely on the option names alone.

After fetching the pinned FLTK commit, CMake replaces the stock target source
surface with an explicit allowlist representing gdupe's required Win32/UI
provider closure. The Win32 print bootstrap is replaced by a one-line no-op,
and printer/PostScript/PDF sources are not compiled. The `fltk_images` target is
reduced to `Fl_Anim_GIF_Image.cxx`, `Fl_GIF_Image.cxx`, and
`Fl_Image_Reader.cxx`; JPEG, PNG, BMP, PNM, and SVG image sources are not
compiled by FLTK because gdupe already owns the required static-image decoders.

Several optional FLTK provider hooks live inside otherwise-required Win32 driver
translation units. A guarded, exact-match patch removes only providers gdupe
cannot call: editable text-input keyboard handling, the Windows Text Editor
Ctrl+Y binding, and FLTK file-browser drive enumeration. This prevents the
otherwise-unused Input, Text Editor, Browser, Message/dialog, Slider/Scrollbar,
and Valuator subtrees from entering the static link.

gdupe also deliberately fixes its FLTK appearance to `gtk+`. The same guarded
patch makes the Plastic, Gleam, and Oxy scheme branches unreachable and removes
the separate Oxy-only arrow dispatch. Linker tracing was used after each cut;
only objects proven unused were removed from the source allowlist. The Plastic,
Gleam, Oxy, and tiled-background implementation sources are therefore absent,
while the generic arrow renderer and GTK drawing implementation remain.

CI audits the generated Visual Studio projects in addition to the FLTK cache
options. It fails if forbidden printer, file-chooser, unused widget/theme, or
FLTK static-image sources re-enter the build, or if the GIF-only image source
set changes. All local FLTK source patches are exact-match guarded against the
pinned upstream revision so a future pin change fails configuration instead of
silently applying stale surgery.

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