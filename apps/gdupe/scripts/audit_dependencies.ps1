$ErrorActionPreference = "Stop"

$workspace = $env:GITHUB_WORKSPACE
$buildRoot = Join-Path $workspace "build\gdupe"
$appRoot = Join-Path $workspace "apps\gdupe"
$statusPath = Join-Path $buildRoot "vcpkg_installed\vcpkg\status"
if (-not (Test-Path -LiteralPath $statusPath -PathType Leaf)) {
  throw "vcpkg status database was not found at $statusPath"
}

$records = foreach ($block in ((Get-Content -LiteralPath $statusPath -Raw) -split "(?:\r?\n){2,}")) {
  $fields = @{}
  foreach ($line in ($block -split "\r?\n")) {
    if ($line -match '^([^:]+):\s*(.*)$') {
      $fields[$Matches[1]] = $Matches[2]
    }
  }
  if ($fields.Package -and $fields.Architecture -eq "x64-windows-static-crt") {
    [pscustomobject]@{
      Package = $fields.Package
      Feature = if ($fields.Feature) { $fields.Feature } else { "core" }
    }
  }
}

$expectedPackages = @(
  "curl",
  "libjpeg-turbo",
  "libwebm",
  "libwebp",
  "nlohmann-json",
  "sqlite3",
  "zlib"
) | Sort-Object
$actualPackages = @(
  $records.Package |
    Where-Object { -not $_.StartsWith("vcpkg-") } |
    Sort-Object -Unique
)
$packageDifference = @(Compare-Object $expectedPackages $actualPackages)
if ($packageDifference.Count -ne 0) {
  throw "Static dependency package surface changed unexpectedly: $($packageDifference | Out-String)"
}

function Assert-Features([string]$package, [string[]]$expected) {
  $actual = @(
    $records |
      Where-Object Package -eq $package |
      ForEach-Object Feature |
      Sort-Object -Unique
  )
  $difference = @(Compare-Object ($expected | Sort-Object) $actual)
  if ($difference.Count -ne 0) {
    throw "$package feature surface changed unexpectedly: $($difference | Out-String)"
  }
}

Assert-Features "curl" @("core", "ssl", "sspi")
Assert-Features "libjpeg-turbo" @("core")
Assert-Features "libwebm" @("core")
Assert-Features "libwebp" @("core", "unicode")
Assert-Features "nlohmann-json" @("core")
Assert-Features "sqlite3" @("core")
Assert-Features "zlib" @("core")

$manifest = Get-Content (Join-Path $appRoot "vcpkg.json") -Raw | ConvertFrom-Json
$declared = @($manifest.dependencies | ForEach-Object {
  if ($_ -is [string]) { $_ } else { $_.name }
} | Sort-Object)
$expectedDeclared = @(
  "curl", "libjpeg-turbo", "libwebm", "libwebp", "nlohmann-json", "sqlite3"
) | Sort-Object
if (@(Compare-Object $expectedDeclared $declared).Count -ne 0) {
  throw "Top-level dependency manifest changed unexpectedly"
}

$triplet = Get-Content (Join-Path $appRoot "cmake\triplets\x64-windows-static-crt.cmake") -Raw
foreach ($setting in @("VCPKG_CRT_LINKAGE static", "VCPKG_LIBRARY_LINKAGE static")) {
  if (-not $triplet.Contains("set($setting)")) {
    throw "Static triplet invariant failed: set($setting)"
  }
}

$cmake = Get-Content (Join-Path $appRoot "CMakeLists.txt") -Raw
foreach ($required in @(
  'gdupe_enable_strict_warnings',
  '/W4 /WX /utf-8',
  'src/jpeg_decode.c',
  'tests/test_jpeg.cpp',
  'gdupe_jpeg_tests',
  'src/wic_gif.cpp',
  'src/preview_color.cpp',
  'src/nvdec_decode.cpp',
  'src/video_demux.cpp',
  'src/mp4_demux.cpp',
  'src/webm_demux.cpp',
  'src/media_decode.cpp',
  'src/preview_decode.cpp',
  'src/video_preview.cpp',
  'target_include_directories(gdupe_minimp4 SYSTEM INTERFACE',
  'd2d1',
  'dwrite',
  'windowscodecs',
  'GIT_TAG 5a212a18dba7dca09543bbc7d65619274fd2931a',
  'GIT_TAG 0a6fba9a2820628b8103464f4c8753ee05838baa'
)) {
  if (-not $cmake.Contains($required)) {
    throw "Native NVIDIA Windows build invariant is missing: $required"
  }
}
foreach ($forbiddenSnippet in @(
  'FetchContent_Declare(fltk',
  'fltk::fltk',
  'fltk::images',
  'Fl_GIF_Image',
  'Fl_Anim_GIF_Image',
  'find_package(PNG',
  'PNG::PNG',
  'NanoSVG',
  'nanosvg',
  'GDUPE_LIBAVC_DIR',
  'GDUPE_LIBHEVC_DIR',
  'AOSP::libavcdec',
  'AOSP::libhevcdec',
  'Dav1d::dav1d',
  'unofficial::libvpx',
  'src/mp4_decode.cpp',
  'mfplay',
  'mfplat',
  'mfuuid'
)) {
  if ($cmake.Contains($forbiddenSnippet)) {
    throw "Retired dependency or decoder residue found in CMake: $forbiddenSnippet"
  }
}

foreach ($retiredSource in @(
  'src\annexb_decoder.hpp',
  'src\mp4_decode.cpp',
  'src\mp4_decode.hpp'
)) {
  if (Test-Path -LiteralPath (Join-Path $appRoot $retiredSource)) {
    throw "Retired decoder source path is still present: $retiredSource"
  }
}

$sourceFiles = Get-ChildItem (Join-Path $appRoot "src") -Recurse -File -Include *.c,*.cpp,*.h,*.hpp
$sourceResidue = @($sourceFiles | Select-String -Pattern '(?i)(?:<FL/|\bFLTK\b|NanoSVG|nanosvg|opencv|libav(?:codec|format|util)|<libav|aosp_(?:avc|hevc)|<dav1d/|dav1d_|<vpx/|vpx_codec_|<mf(?:api|play)\.h>|MFStartup|MFShutdown|IMFPMediaPlayer|MFPCreateMediaPlayer|\bAnnexBDecoder\b|\bdecode_mp4_static\b)')
if ($sourceResidue.Count -ne 0) {
  throw "Retired dependency/decoder residue found in production source: $($sourceResidue | Out-String)"
}

$cppSourceFiles = Get-ChildItem (Join-Path $appRoot "src") -Recurse -File -Include *.cpp,*.hpp
$jpegCppResidue = @($cppSourceFiles | Select-String -Pattern '(?i)(?:<jpeglib\.h>|\bsetjmp\b|\blongjmp\b)')
if ($jpegCppResidue.Count -ne 0) {
  throw "libjpeg long-jump boundary leaked back into C++ source: $($jpegCppResidue | Out-String)"
}
$previewChecksumResidue = @($sourceFiles | Select-String -Pattern '\bpreview_checksum\b')
if ($previewChecksumResidue.Count -ne 0) {
  throw "Test-only preview checksum leaked into production source: $($previewChecksumResidue | Out-String)"
}

$generatedProjects = @(Get-ChildItem $buildRoot -File -Filter *.vcxproj)
$generatedResidue = @($generatedProjects | Select-String -Pattern '(?i)fltk|nanosvg|opencv|libpng|libavc|libhevc|dav1d|(?:^|[\\/])vpx(?:\.lib|[\\/])|mfplay\.lib|mfplat\.lib|mfuuid\.lib')
if ($generatedResidue.Count -ne 0) {
  throw "Retired dependency found in generated Visual Studio projects: $($generatedResidue | Out-String)"
}

$thirdPartyRoot = Join-Path $appRoot "third_party"
if (Test-Path -LiteralPath (Join-Path $thirdPartyRoot "aosp")) {
  throw "Retired AOSP decoder adaptation tree is still present"
}

$nvdecSource = Get-Content (Join-Path $appRoot "src\nvdec_decode.cpp") -Raw
foreach ($required in @(
  "nvcuda.dll",
  "nvcuvid.dll",
  "cuvidGetDecoderCaps",
  "cudaVideoCodec_HEVC",
  "cudaVideoCodec_AV1",
  "cudaVideoSurfaceFormat_P016",
  "macroblock_count",
  "requires decoder reconfiguration",
  "cuMemcpy2D preview chroma"
)) {
  if (-not $nvdecSource.Contains($required)) {
    throw "NVDEC runtime/preview boundary is incomplete: $required"
  }
}

$demuxFactory = Get-Content (Join-Path $appRoot "src\video_demux.cpp") -Raw
foreach ($required in @(
  "open_mp4_video_demux",
  "open_webm_video_demux",
  "open_video_demux"
)) {
  if (-not $demuxFactory.Contains($required)) {
    throw "Shared video demux factory is incomplete: $required"
  }
}

$mp4Demux = Get-Content (Join-Path $appRoot "src\mp4_demux.cpp") -Raw
foreach ($required in @(
  "MINIMP4_IMPLEMENTATION",
  "sample_to_annexb",
  "info_.width = codec_.width",
  "info_.height = codec_.height",
  "open_mp4_video_demux"
)) {
  if (-not $mp4Demux.Contains($required)) {
    throw "MP4 demux boundary is incomplete: $required"
  }
}

$webmDemux = Get-Content (Join-Path $appRoot "src\webm_demux.cpp") -Raw
foreach ($required in @(
  "mkvparser::Cluster::Create",
  "parse_track_entry",
  "V_AV1",
  "open_webm_video_demux"
)) {
  if (-not $webmDemux.Contains($required)) {
    throw "WebM demux boundary is incomplete: $required"
  }
}
if ($webmDemux.Contains("Segment::Load") -or
    $webmDemux.Contains("ParseHeaders")) {
  throw "WebM demux regressed to libwebm's strict whole-header/segment parser"
}

$mainWindow = Get-Content (Join-Path $appRoot "src\main_window.cpp") -Raw
if (-not $mainWindow.Contains("VideoPreview") -or
    -not $mainWindow.Contains("fit_preview_rect")) {
  throw "Native preview is not wired into the Direct2D review panes"
}

Write-Host "Exact dependency surface verified: warning-clean native Windows UI, isolated C libjpeg error handling, WIC image paths, one shared packet-demux interface with isolated MP4/WebM implementations, static redistributable libraries, and one NVIDIA-driver NVDEC stack for video analysis and preview."
