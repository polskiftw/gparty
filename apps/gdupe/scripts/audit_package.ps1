$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$root = Join-Path $env:GITHUB_WORKSPACE "dist\gdupe"
$required = @(
  "gdupe.exe",
  "gfingerd.exe",
  "LICENSE",
  "README.md",
  "GFINGERD-README.md",
  "config\gdupe.example.json",
  "config\gfingerd.example.json",
  "licenses\minimp4\LICENSE.txt",
  "licenses\nv-codec-headers\LICENSE.txt",
  "licenses\curl\LICENSE.txt",
  "licenses\libjpeg-turbo\LICENSE.md",
  "licenses\libjpeg-turbo\README.ijg",
  "licenses\libwebm\LICENSE.txt",
  "licenses\libwebm\PATENTS.txt",
  "licenses\libwebp\LICENSE.txt",
  "licenses\nlohmann-json\LICENSE.txt",
  "licenses\sqlite3\PUBLIC-DOMAIN.txt",
  "licenses\zlib\LICENSE.txt"
)
foreach ($path in $required) {
  if (-not (Test-Path -LiteralPath (Join-Path $root $path) -PathType Leaf)) {
    throw "Application package is missing $path"
  }
}

foreach ($obsolete in @("RUNTIME-SURFACE.md", "THIRD-PARTY-NOTICES.md", "FLTK-SOURCE.txt")) {
  if (Test-Path -LiteralPath (Join-Path $root $obsolete)) {
    throw "Application package contains obsolete root documentation: $obsolete"
  }
}
foreach ($retiredLicense in @("fltk", "libpng", "libavc", "libhevc", "dav1d", "libvpx")) {
  if (Test-Path -LiteralPath (Join-Path $root "licenses\$retiredLicense")) {
    throw "Application package contains stale retired dependency material: licenses\$retiredLicense"
  }
}

if (Test-Path -LiteralPath (Join-Path $root "licenses\vcpkg")) {
  throw "Application package contains the obsolete vcpkg-internal legal layout"
}
$rawCopyrightFiles = @(Get-ChildItem (Join-Path $root "licenses") -Recurse -File -Filter "copyright")
if ($rawCopyrightFiles.Count -ne 0) {
  throw "Application package contains unstandardized vcpkg copyright files: $($rawCopyrightFiles.FullName -join ', ')"
}

$legalDocs = @(
  Get-ChildItem (Join-Path $root "licenses") -Recurse -File |
    Where-Object { $_.Name -match '^(?:LICENSE(?:\..+)?|COPYING(?:\..+)?|PUBLIC-DOMAIN(?:\..+)?|NOTICE(?:\..+)?|PATENTS(?:\..+)?)$' }
)
foreach ($legal in $legalDocs) {
  if ($legal.Length -lt 100) {
    throw "Legal document is suspiciously short or pointer-only: $($legal.FullName) ($($legal.Length) bytes)"
  }
}

$jpegLicense = Get-Content -LiteralPath (Join-Path $root "licenses\libjpeg-turbo\LICENSE.md") -Raw
if (-not $jpegLicense.Contains("The Modified (3-clause) BSD License") -or
    -not $jpegLicense.Contains("Redistribution and use in source and binary forms")) {
  throw "libjpeg-turbo BSD license text is incomplete"
}
$ijgLicense = Get-Content -LiteralPath (Join-Path $root "licenses\libjpeg-turbo\README.ijg") -Raw
if (-not $ijgLicense.Contains("LEGAL ISSUES") -or
    -not $ijgLicense.Contains("Permission is hereby granted to use, copy, modify, and distribute this")) {
  throw "Independent JPEG Group license terms are incomplete"
}
$sqliteNotice = Get-Content -LiteralPath (Join-Path $root "licenses\sqlite3\PUBLIC-DOMAIN.txt") -Raw
if (-not $sqliteNotice.Contains("public domain") -or
    -not $sqliteNotice.Contains("copy, modify, publish, use, compile, sell, or distribute") -or
    -not $sqliteNotice.Contains("no license is required")) {
  throw "SQLite public-domain notice is incomplete"
}
$webmPatents = Get-Content -LiteralPath (Join-Path $root "licenses\libwebm\PATENTS.txt") -Raw
if (-not $webmPatents.Contains("Additional IP Rights Grant (Patents)")) {
  throw "libwebm patent grant is incomplete"
}
$nvHeaderLicense = Get-Content -LiteralPath (Join-Path $root "licenses\nv-codec-headers\LICENSE.txt") -Raw
if (-not $nvHeaderLicense.Contains("Permission is hereby granted, free of charge") -or
    -not $nvHeaderLicense.Contains('THE SOFTWARE IS PROVIDED "AS IS"')) {
  throw "nv-codec-headers MIT license text is incomplete"
}

$readme = Get-Content -LiteralPath (Join-Path $root "README.md") -Raw
if (-not $readme.Contains("This software is based in part on the work of the Independent JPEG Group.")) {
  throw "Required Independent JPEG Group acknowledgement is missing from README.md"
}
if ($readme -match '(?i)\bportable\b') {
  throw "README.md still describes gdupe as portable"
}
if (-not $readme.Contains("NVIDIA") -or -not $readme.Contains("NVDEC")) {
  throw "README.md does not document the NVIDIA NVDEC runtime requirement"
}
if ($readme -match '(?i)Media Foundation|MFPlay') {
  throw "README.md still documents the retired Media Foundation preview path"
}
if (-not $readme.Contains("Video preview | NVIDIA NVDEC")) {
  throw "README.md does not document native NVDEC video preview"
}

# No redistributable or NVIDIA driver DLL is copied beside the application.
# nvcuda.dll and nvcuvid.dll are resolved from the installed NVIDIA driver at
# runtime with LoadLibrary; they are neither linked into nor shipped with gdupe.
$actualDlls = @(Get-ChildItem $root -Recurse -File -Filter "*.dll")
if ($actualDlls.Count -ne 0) {
  throw "Application package must contain zero DLLs, but found: $($actualDlls.FullName -join ', ')"
}

$forbidden = @(
  Get-ChildItem $root -Recurse -File |
    Where-Object {
      $_.Name -in "ffmpeg.exe", "ffprobe.exe", "ffplay.exe", "VC_redist.x64.exe", "qt.conf" -or
      $_.Name -match '^(?:Qt6|opencv|fltk|avcodec|avformat|avutil|swscale|avfilter|avdevice|swresample).*\.dll$' -or
      $_.Name -match '^(?:concrt|msvcp|vcruntime|msvcr|ucrtbase).*\.dll$'
    }
)
if ($forbidden.Count -ne 0) {
  throw "Application package contains forbidden runtime files: $($forbidden.FullName -join ', ')"
}
if (Test-Path -LiteralPath (Join-Path $root "plugins")) {
  throw "Application package contains an unexpected plugin tree"
}
if (Test-Path -LiteralPath (Join-Path $root "tools")) {
  throw "Application package contains an unexpected tools tree"
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

$dynamicCrtPattern = '(?i)^(?:concrt|msvcp|vcruntime|msvcr|ucrtbase).*\.dll$'
$thirdPartyNamePattern = '(?i)^(?:avcodec|avformat|avutil|swscale|avfilter|avdevice|swresample|Qt6|opencv|fltk|libavc|libhevc|dav1d|vpx|webm|webp|jpeg|png|zlib|sqlite|curl).*\.dll$'
$mediaFoundationPattern = '(?i)^(?:mf|mfplat|mfplay|mfreadwrite|mfuuid)\.dll$'
foreach ($application in @("gdupe.exe", "gfingerd.exe")) {
  $exe = Join-Path $root $application
  $dependentText = (& $dumpbin.FullName /nologo /dependents $exe 2>&1 | Out-String)
  $dependentText | Write-Host
  $imports = @(
    [regex]::Matches($dependentText, '(?im)^\s*([A-Za-z0-9_.+\-]+\.dll)\s*$') |
      ForEach-Object { $_.Groups[1].Value } |
      Sort-Object -Unique
  )
  if ($imports.Count -eq 0) {
    throw "Could not enumerate $application DLL imports"
  }
  foreach ($dll in $imports) {
    if ($dll -match $dynamicCrtPattern) {
      throw "$application imports the dynamic MSVC/UCRT runtime: $dll"
    }
    if ($dll -match $thirdPartyNamePattern) {
      throw "$application dynamically imports a redistributable third-party library: $dll"
    }
    if ($dll -match $mediaFoundationPattern) {
      throw "$application directly imports retired Media Foundation runtime: $dll"
    }
    if ($dll -match '(?i)^(?:nvcuda|nvcuvid)\.dll$') {
      throw "NVIDIA driver DLL must be runtime-loaded, not linked as an application import: $dll"
    }
    if ($dll -match '(?i)^(?:api-ms-win-|ext-ms-win-)') {
      continue
    }
    $systemDll = Join-Path (Join-Path $env:SystemRoot "System32") $dll
    if (-not (Test-Path -LiteralPath $systemDll -PathType Leaf)) {
      throw "$application imports non-system DLL $dll; redistributable dependencies must be static"
    }
  }
}

$archive = Join-Path $env:GITHUB_WORKSPACE "dist\gdupe-windows-x64.zip"
if (Test-Path -LiteralPath $archive) {
  Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -Path (Join-Path $root "*") -DestinationPath $archive
Write-Host "Application package verified: native executables, zero shipped DLLs, static redistributable graph, one shared NVIDIA NVDEC/fingerprint stack, consolidated legal texts."
