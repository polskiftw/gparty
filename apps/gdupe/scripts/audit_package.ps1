$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$root = Join-Path $env:GITHUB_WORKSPACE "dist\gdupe"
$required = @(
  "gdupe.exe",
  "config\gdupe.example.json",
  "avcodec-63.dll",
  "avformat-63.dll",
  "avutil-61.dll",
  "swscale-10.dll",
  "licenses\ffmpeg\LICENSE.txt",
  "licenses\ffmpeg\LICENSE-NOTICE.md",
  "licenses\ffmpeg\SOURCE.txt",
  "licenses\ffmpeg\BUILD-CONFIG.txt",
  "licenses\fltk\COPYING",
  "licenses\fltk\FLTK-SOURCE.txt",
  "licenses\vcpkg\curl\copyright",
  "licenses\vcpkg\libjpeg-turbo\copyright",
  "licenses\vcpkg\libpng\copyright",
  "licenses\vcpkg\libwebp\copyright",
  "licenses\vcpkg\nlohmann-json\copyright",
  "licenses\vcpkg\sqlite3\copyright",
  "licenses\vcpkg\zlib\copyright",
  "RUNTIME-SURFACE.md",
  "README.md"
)
foreach ($path in $required) {
  if (-not (Test-Path -LiteralPath (Join-Path $root $path) -PathType Leaf)) {
    throw "Portable package is missing $path"
  }
}

$expectedDlls = @(
  "avcodec-63.dll",
  "avformat-63.dll",
  "avutil-61.dll",
  "swscale-10.dll"
) | Sort-Object
$actualDlls = @(
  Get-ChildItem $root -Recurse -File -Filter "*.dll" |
    ForEach-Object Name |
    Sort-Object
)
$dllDifference = @(Compare-Object $expectedDlls $actualDlls)
if ($dllDifference.Count -ne 0) {
  throw "Portable DLL surface changed unexpectedly: $($dllDifference | Out-String)"
}

$forbidden = @(
  Get-ChildItem $root -Recurse -File |
    Where-Object {
      $_.Name -in "ffmpeg.exe", "ffprobe.exe", "VC_redist.x64.exe", "qt.conf" -or
      $_.Name -match '^(?:Qt6|opencv|fltk|avfilter|avdevice|swresample).*\.dll$' -or
      $_.Name -match '^(?:concrt|msvcp|vcruntime|msvcr)\d.*\.dll$'
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

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = (& $vswhere -latest -products * -property installationPath).Trim()
$dumpbin = Get-ChildItem "$vsInstall\VC\Tools\MSVC" -Recurse -File -Filter dumpbin.exe |
  Where-Object FullName -Match '\\Hostx64\\x64\\dumpbin\.exe$' |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $dumpbin) {
  throw "dumpbin.exe was not found"
}

$runtimeImports = @()
Get-ChildItem $root -Recurse -File |
  Where-Object Extension -In ".exe", ".dll" |
  ForEach-Object {
    $imports = & $dumpbin.FullName /nologo /dependents $_.FullName |
      Select-String -Pattern '(?i)\b(?:concrt|msvcp|vcruntime|msvcr)\d[^\s]*\.dll\b'
    if ($imports) {
      $runtimeImports += "$($_.FullName): $($imports -join ', ')"
    }
  }
if ($runtimeImports.Count -ne 0) {
  throw "Portable package imports the dynamic MSVC runtime:$([Environment]::NewLine)$($runtimeImports -join [Environment]::NewLine)"
}

$gdupeImports = (& $dumpbin.FullName /nologo /dependents (Join-Path $root "gdupe.exe")) -join [Environment]::NewLine
foreach ($dll in $expectedDlls) {
  if ($gdupeImports -notmatch "(?im)^\s*$([regex]::Escape($dll))\s*$") {
    throw "gdupe.exe does not directly import required minimal FFmpeg DLL $dll"
  }
}
if ($gdupeImports -match '(?i)(?:Qt6|opencv|fltk).*\.dll') {
  throw "gdupe.exe unexpectedly imports a Qt, OpenCV, or FLTK DLL"
}

$archive = Join-Path $env:GITHUB_WORKSPACE "dist\gdupe-windows-x64.zip"
Compress-Archive -Path (Join-Path $root "*") -DestinationPath $archive
Write-Host "Portable package contains gdupe.exe and exactly four FFmpeg DLLs."
