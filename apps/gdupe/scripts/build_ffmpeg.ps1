$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

foreach ($name in @(
  "GDUPE_FFMPEG_DIR",
  "FFMPEG_SOURCE_COMMIT",
  "RUNNER_TEMP"
)) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "Required environment variable is missing: $name"
  }
}

$bash = "C:\msys64\usr\bin\bash.exe"
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
  throw "MSYS2 bash was not found at $bash"
}
& $bash -lc 'pacman -S --noconfirm --needed make diffutils nasm'

$source = Join-Path $env:RUNNER_TEMP "gdupe-ffmpeg-src"
git clone --filter=blob:none https://github.com/FFmpeg/FFmpeg.git $source
git -C $source checkout --detach $env:FFMPEG_SOURCE_COMMIT

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = (& $vswhere -latest -products * -property installationPath).Trim()
if ([string]::IsNullOrWhiteSpace($vsInstall)) {
  throw "Visual Studio was not found"
}
Import-Module (Join-Path $vsInstall "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation `
  -DevCmdArguments "-arch=x64 -host_arch=x64"

$env:GDUPE_FFMPEG_SOURCE = $source
New-Item -ItemType Directory -Force $env:GDUPE_FFMPEG_DIR | Out-Null

& $bash -lc @'
set -euo pipefail
vcbin="$(cygpath -u "$VCToolsInstallDir")/bin/Hostx64/x64"
export PATH="$vcbin:/usr/bin:$PATH"
command -v cl.exe
command -v link.exe
command -v lib.exe
command -v nasm

src="$(cygpath -u "$GDUPE_FFMPEG_SOURCE")"
prefix="$(cygpath -u "$GDUPE_FFMPEG_DIR")"
cd "$src"
# Visual Studio 2026 emits multiple banner lines matching this older
# configure probe. Limit the probe to its first numeric compiler version.
sed -i "/cl_major_ver=.*cl\\.exe/ s@p')@p' | head -n 1)@" configure
grep -F "head -n 1" configure >/dev/null
./configure \
  --toolchain=msvc \
  --arch=x86_64 \
  --prefix="$prefix" \
  --bindir="$prefix" \
  --shlibdir="$prefix" \
  --libdir="$prefix/lib" \
  --incdir="$prefix/include" \
  --disable-static \
  --enable-shared \
  --extra-cflags=-MT \
  --enable-small \
  --disable-autodetect \
  --disable-everything \
  --disable-programs \
  --disable-network \
  --disable-avdevice \
  --disable-avfilter \
  --disable-swresample \
  --enable-swscale \
  --disable-doc \
  --disable-debug \
  --disable-iconv \
  --disable-bzlib \
  --disable-lzma \
  --disable-zlib \
  --disable-pthreads \
  --enable-w32threads \
  --enable-protocol=file \
  --enable-demuxer=gif \
  --enable-demuxer=matroska \
  --enable-demuxer=mov \
  --enable-decoder=av1 \
  --enable-decoder=gif \
  --enable-decoder=h264 \
  --enable-decoder=hevc \
  --enable-decoder=vp8 \
  --enable-decoder=vp9

make -j"${NUMBER_OF_PROCESSORS:-4}"
make install
mkdir -p "$prefix/lib"
for import_library in avcodec.lib avformat.lib avutil.lib swscale.lib; do
  test -f "$prefix/$import_library"
  mv "$prefix/$import_library" "$prefix/lib/$import_library"
done
cp "$src/COPYING.LGPLv2.1" "$prefix/LICENSE.txt"
cp "$src/LICENSE.md" "$prefix/LICENSE-NOTICE.md"
{
  printf '%s\n' '=== ffbuild/config.mak ==='
  cat "$src/ffbuild/config.mak"
  printf '%s\n' '=== config_components.h ==='
  cat "$src/config_components.h"
} > "$prefix/BUILD-CONFIG.txt"
printf 'FFmpeg source: https://github.com/FFmpeg/FFmpeg\nPinned commit: %s\nBuild recipe: apps/gdupe/scripts/build_ffmpeg.ps1\nCompiler: Microsoft Visual C++ with the static /MT runtime\n' \
  "$FFMPEG_SOURCE_COMMIT" > "$prefix/SOURCE.txt"
'@
