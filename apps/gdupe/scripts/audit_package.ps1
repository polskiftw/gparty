$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$root = Join-Path $env:GITHUB_WORKSPACE "dist\gdupe"
$required = @(
  "gdupe.exe",
  "LICENSE",
  "config\gdupe.example.json",
  "licenses\libavc\LICENSE.txt",
  "licenses\libavc\SOURCE.txt",
  "licenses\libhevc\LICENSE.txt",
  "licenses\libhevc\SOURCE.txt",
  "licenses\minimp4\LICENSE.txt",
  "licenses\fltk\LICENSE.txt",
  "licenses\fltk\SOURCE.txt",
  "licenses\curl\LICENSE.txt",
  "licenses\dav1d\LICENSE.txt",
  "licenses\libjpeg-turbo\LICENSE.md",
  "licenses\libjpeg-turbo\README.ijg",
  "licenses\libpng\LICENSE.txt",
  "licenses\libvpx\LICENSE.txt",
  "licenses\libwebm\LICENSE.txt",
  "licenses\libwebp\LICENSE.txt",
  "licenses\nlohmann-json\LICENSE.txt",
  "licenses\sqlite3\PUBLIC-DOMAIN.txt",
  "licenses\zlib\LICENSE.txt",
  "RUNTIME-SURFACE.md",
  "THIRD-PARTY-NOTICES.md",
  "README.md"
)
foreach ($path in $required) {
  if (-not (Test-Path -LiteralPath (Join-Path $root $path) -PathType Leaf)) {
    throw "Portable package is missing $path"
  }
}

# Keep the distribution-facing license tree deliberate rather than exposing
# vcpkg's internal share/copyright layout.
if (Test-Path -LiteralPath (Join-Path $root "licenses\vcpkg")) {
  throw "Portable package contains the obsolete vcpkg-internal license layout"
}
$rawCopyrightFiles = @(Get-ChildItem (Join-Path $root "licenses") -Recurse -File -Filter "copyright")
if ($rawCopyrightFiles.Count -ne 0) {
  throw "Portable package contains unstandardized vcpkg copyright files: $($rawCopyrightFiles.FullName -join ', ')"
}

# A license file that is only a URL or one-line pointer is not acceptable for
# this package. Every distribution-facing license/public-domain document must
# carry substantive terms locally.
$licenseDocs = @(
  Get-ChildItem (Join-Path $root "licenses") -Recurse -File |
    Where-Object { $_.Name -match '^(?:LICENSE(?:\..+)?|COPYING(?:\..+)?|PUBLIC-DOMAIN(?:\..+)?)$' }
)
foreach ($license in $licenseDocs) {
  if ($license.Length -lt 400) {
    throw "License document is suspiciously short/pointer-only: $($license.FullName) ($($license.Length) bytes)"
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
$thirdPartyNotices = Get-Content -LiteralPath (Join-Path $root "THIRD-PARTY-NOTICES.md") -Raw
if (-not $thirdPartyNotices.Contains("This software is based in part on the work of the Independent JPEG Group.")) {
  throw "Required Independent JPEG Group acknowledgement is missing"
}

# The distribution contract is deliberately stronger than merely avoiding
# FFmpeg: no third-party DLL is shipped at all. Every redistributable library
# must be linked into gdupe.exe.
$actualDlls = @(Get-ChildItem $root -Recurse -File -Filter "*.dll")
if ($actualDlls.Count -ne 0) {
  throw "Portable package must contain zero DLLs, but found: $($actualDlls.FullName -join ', ')"
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
  throw "Portable package contains forbidden runtime files: $($forbidden.FullName -join ', ')"
}
if (Test-Path -LiteralPath (Join-Path $root "plugins")) {
  throw "Portable package contains an unexpected plugin tree"
}
if (Test-Path -LiteralPath (Join-Path $root "tools")) {
  throw "Portable package contains an unexpected tools tree"
}
if (Test-Path -LiteralPath (Join-Path $root "licenses\ffmpeg")) {
  throw "Portable package still contains the retired FFmpeg license tree"
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

$exe = Join-Path $root "gdupe.exe"
$dependentText = (& $dumpbin.FullName /nologo /dependents $exe 2>&1 | Out-String)
$dependentText | Write-Host
$imports = @(
  [regex]::Matches($dependentText, '(?im)^\s*([A-Za-z0-9_.+\-]+\.dll)\s*$') |
    ForEach-Object { $_.Groups[1].Value } |
    Sort-Object -Unique
)
if ($imports.Count -eq 0) {
  throw "Could not enumerate gdupe.exe DLL imports"
}

$dynamicCrtPattern = '(?i)^(?:concrt|msvcp|vcruntime|msvcr|ucrtbase).*\.dll$'
$thirdPartyNamePattern = '(?i)^(?:avcodec|avformat|avutil|swscale|avfilter|avdevice|swresample|Qt6|opencv|fltk|libavc|libhevc|dav1d|vpx|webm|webp|jpeg|png|zlib|sqlite|curl).*\.dll$'
foreach ($dll in $imports) {
  if ($dll -match $dynamicCrtPattern) {
    throw "gdupe.exe imports the dynamic MSVC/UCRT runtime: $dll"
  }
  if ($dll -match $thirdPartyNamePattern) {
    throw "gdupe.exe dynamically imports a third-party library: $dll"
  }

  # Windows API-set imports are virtual contracts resolved by Windows itself.
  if ($dll -match '(?i)^(?:api-ms-win-|ext-ms-win-)') {
    continue
  }

  # Everything else directly imported by gdupe.exe must be an actual Windows
  # system DLL. A stray third-party import therefore fails even if its name is
  # not in the explicit denylist above.
  $systemDll = Join-Path (Join-Path $env:SystemRoot "System32") $dll
  if (-not (Test-Path -LiteralPath $systemDll -PathType Leaf)) {
    throw "gdupe.exe imports non-system DLL $dll; all redistributable dependencies must be static"
  }
}

$archive = Join-Path $env:GITHUB_WORKSPACE "dist\gdupe-windows-x64.zip"
if (Test-Path -LiteralPath $archive) {
  Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -Path (Join-Path $root "*") -DestinationPath $archive
Write-Host "Portable package verified: one executable, zero shipped DLLs, complete local license texts, static third-party dependency graph."
