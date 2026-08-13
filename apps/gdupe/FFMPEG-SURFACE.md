# gdupe FFmpeg surface audit

This document is the allowlist for gdupe's pinned FFmpeg build. The build starts with `--disable-autodetect --disable-everything`, enables only the items below, and produces shared libraries only. CI compares FFmpeg's generated `config_components.h` against the exact final component set and fails on any addition or removal.

## Required libraries

| DLL | Why gdupe needs it | APIs used |
| --- | --- | --- |
| `avformat-63.dll` | Open a local media file, enforce the bounded read callback, inspect its streams, and demux video packets | `avformat_alloc_context`, `avformat_open_input`, `avformat_find_stream_info`, `av_find_best_stream`, `av_read_frame`, `av_guess_frame_rate` |
| `avcodec-63.dll` | Create the selected decoder and turn video packets into frames | `avcodec_alloc_context3`, `avcodec_parameters_to_context`, `avcodec_open2`, `avcodec_send_packet`, `avcodec_receive_frame` |
| `avutil-61.dll` | FFmpeg allocation, packet/frame lifetime, rational time conversion, and error text shared by the libraries above | `av_frame_alloc`, `av_frame_free`, `av_packet_alloc`, `av_packet_free`, `av_q2d`, `av_strerror` |
| `swscale-10.dll` | Convert the decoder's possible YUV/RGB pixel layouts to BGR24 for the existing OpenCV fingerprint code | `sws_getCachedContext`, `sws_scale`, `sws_freeContext` |

`swscale` is used for pixel-format conversion only. gdupe does not resize video frames through FFmpeg.

## Required input components

| Type | Enabled components | Reason |
| --- | --- | --- |
| Protocol | `file` | gdupe fingerprints a staged local file; FFmpeg network access is neither used nor allowed |
| Demuxers | `gif`, `mov`, `matroska` | GIF animation; MP4 and containerized M4V; WebM |
| Decoders | `gif`, `h264`, `hevc`, `vp8`, `vp9`, `av1` | Codecs used by the accepted GIF, MP4/M4V, and WebM extension families |
| Parser | `vp9` | Automatically selected by and required for FFmpeg's native VP9 decoder |
| Bitstream filter | `vp9_superframe_split` | Automatically selected by and required for FFmpeg's native VP9 decoder |

The `mov` demuxer is FFmpeg's shared ISO Base Media implementation for MP4/M4V as well as MOV. Enabling it does not mean gdupe accepts `.mov`; the application extension allowlist remains JPEG, PNG, WebP, GIF, MP4, M4V, and WebM. Likewise, FFmpeg's Matroska demuxer is the implementation required for WebM while gdupe does not accept `.mkv`.

## Explicitly absent

- Programs: `ffmpeg.exe`, `ffprobe.exe`, and `ffplay.exe`
- Libraries: `avfilter`, `avdevice`, `swresample`, and `postproc`
- Every encoder, muxer, filter, input device, output device, and hardware accelerator
- Every network protocol
- External codec libraries and compression helpers, including zlib, bzip2, and LZMA
- Raw-M4V demuxing, MPEG-4 Part 2 decoding, and their parser dependencies
- GPL, LGPLv3-only, nonfree, or version-3 build switches
- Dynamic MSVC runtime DLLs and MinGW helper runtimes

The removal of the old CLI/temporary-PNG pipeline is what makes the absent encoder, muxer, filters, `avfilter` DLL, zlib, and two FFmpeg programs possible.

## Build and distribution policy

FFmpeg is pinned to commit `6bbc22dc09c214b2f5334afa30167fa1990eb5df`, compiled with Microsoft's C compiler as LGPL 2.1-or-later shared DLLs, and forced to the static `/MT` Microsoft runtime. The portable package includes the FFmpeg license, upstream license notice, exact source commit, build recipe location, and generated build configuration under `licenses/ffmpeg/`.

The Windows workflow rejects:

1. Any FFmpeg DLL other than the four named above.
2. Any enabled decoder, demuxer, parser, protocol, encoder, muxer, filter, or bitstream filter outside the exact component allowlist.
3. GPL/nonfree/version-3 configuration expansion.
4. Imports of `avfilter`, `avdevice`, `swresample`, zlib, WinPthreads, MinGW runtimes, or dynamic MSVC runtime DLLs.
5. A package containing the obsolete FFmpeg programs or `tools/` directory.
