$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$root = $env:GDUPE_FFMPEG_DIR
if ([string]::IsNullOrWhiteSpace($root)) {
  throw "GDUPE_FFMPEG_DIR is missing"
}

$expectedDlls = @(
  "avcodec-63.dll",
  "avformat-63.dll",
  "avutil-61.dll",
  "swscale-10.dll"
) | Sort-Object
$actualDlls = @(
  Get-ChildItem $root -File -Filter "*.dll" |
    ForEach-Object Name |
    Sort-Object
)
$dllDiff = @(Compare-Object $expectedDlls $actualDlls)
if ($dllDiff.Count -ne 0) {
  throw "FFmpeg DLL surface changed unexpectedly: $($dllDiff | Out-String)"
}

foreach ($file in @(
  "lib\avcodec.lib",
  "lib\avformat.lib",
  "lib\avutil.lib",
  "lib\swscale.lib",
  "include\libavcodec\avcodec.h",
  "include\libavformat\avformat.h",
  "include\libavutil\avutil.h",
  "include\libswscale\swscale.h",
  "LICENSE.txt",
  "LICENSE-NOTICE.md",
  "SOURCE.txt",
  "BUILD-CONFIG.txt"
)) {
  if (-not (Test-Path -LiteralPath (Join-Path $root $file) -PathType Leaf)) {
    throw "Minimal FFmpeg SDK is missing $file"
  }
}

$programs = @(
  Get-ChildItem $root -File |
    Where-Object Name -In "ffmpeg.exe", "ffprobe.exe"
)
if ($programs.Count -ne 0) {
  throw "The library-only FFmpeg runtime unexpectedly contains a program"
}

$buildConfigPath = Join-Path $root "BUILD-CONFIG.txt"
$components = Get-Content $buildConfigPath
$enabled = @(
  $components |
    Select-String '^#define CONFIG_([A-Z0-9_]+_(?:DECODER|DEMUXER|PARSER|PROTOCOL|ENCODER|MUXER|FILTER|BSF)) 1$' |
    ForEach-Object { $_.Matches[0].Groups[1].Value } |
    Sort-Object -Unique
)
$expectedComponents = @(
  "AV1_DECODER",
  "FILE_PROTOCOL",
  "GIF_DECODER",
  "GIF_DEMUXER",
  "H264_DECODER",
  "HEVC_DECODER",
  "MATROSKA_DEMUXER",
  "MOV_DEMUXER",
  "VP8_DECODER",
  "VP9_DECODER",
  "VP9_PARSER",
  "VP9_SUPERFRAME_SPLIT_BSF"
) | Sort-Object
$componentDiff = @(Compare-Object $expectedComponents $enabled)
if ($componentDiff.Count -ne 0) {
  throw "FFmpeg capability whitelist changed unexpectedly: $($componentDiff | Out-String)"
}

$configuration = Get-Content $buildConfigPath -Raw
foreach ($flag in @(
  "--toolchain=msvc",
  "--disable-static",
  "--enable-shared",
  "--extra-cflags=-MT",
  "--disable-autodetect",
  "--disable-everything",
  "--disable-programs",
  "--disable-network",
  "--disable-avdevice",
  "--disable-avfilter",
  "--disable-swresample",
  "--enable-swscale",
  "--disable-zlib",
  "--disable-pthreads",
  "--enable-w32threads"
)) {
  if (-not $configuration.Contains($flag)) {
    throw "FFmpeg build policy is missing $flag"
  }
}
if ($configuration -match '--enable-(?:gpl|nonfree|version3)\b') {
  throw "FFmpeg licensing surface unexpectedly expanded"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = (& $vswhere -latest -products * -property installationPath).Trim()
$dumpbin = Get-ChildItem "$vsInstall\VC\Tools\MSVC" -Recurse -File -Filter dumpbin.exe |
  Where-Object FullName -Match '\\Hostx64\\x64\\dumpbin\.exe$' |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $dumpbin) {
  throw "dumpbin.exe was not found"
}

$forbiddenImports = '(?i)\b(?:avfilter-\d+|avdevice-\d+|swresample-\d+|zlib1|libwinpthread-1|libgcc[^\s]*|libstdc\+\+[^\s]*|(?:concrt|msvcp|vcruntime|msvcr)\d[^\s]*)\.dll\b'
$badImports = @()
Get-ChildItem $root -File -Filter "*.dll" |
  Sort-Object Name |
  ForEach-Object {
    Write-Host "=== $($_.Name) dependencies ==="
    $imports = & $dumpbin.FullName /nologo /dependents $_.FullName
    $imports | Out-Host
    $hits = $imports | Select-String -Pattern $forbiddenImports
    if ($hits) {
      $badImports += "$($_.Name): $($hits -join ', ')"
    }
  }
if ($badImports.Count -ne 0) {
  throw "Minimal FFmpeg DLLs import forbidden libraries:$([Environment]::NewLine)$($badImports -join [Environment]::NewLine)"
}
