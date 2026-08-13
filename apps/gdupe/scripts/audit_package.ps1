$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$root = Join-Path $env:GITHUB_WORKSPACE "dist\gdupe"
$required = @(
  "gdupe.exe",
  "qt.conf",
  "Qt6Core.dll",
  "plugins\platforms\qwindows.dll",
  "plugins\multimedia\windowsmediaplugin.dll",
  "config\gdupe.example.json",
  "avcodec-63.dll",
  "avformat-63.dll",
  "avutil-61.dll",
  "swscale-10.dll",
  "licenses\ffmpeg\LICENSE.txt",
  "licenses\ffmpeg\LICENSE-NOTICE.md",
  "licenses\ffmpeg\SOURCE.txt",
  "licenses\ffmpeg\BUILD-CONFIG.txt",
  "README.md"
)
foreach ($path in $required) {
  if (-not (Test-Path -LiteralPath (Join-Path $root $path) -PathType Leaf)) {
    throw "Portable package is missing $path"
  }
}

if (Test-Path -LiteralPath (Join-Path $root "tools")) {
  throw "Portable package still contains the obsolete FFmpeg tools directory"
}
$obsolete = @(
  Get-ChildItem $root -Recurse -File |
    Where-Object Name -In "ffmpeg.exe", "ffprobe.exe", "avfilter-12.dll"
)
if ($obsolete.Count -ne 0) {
  throw "Portable package contains an obsolete FFmpeg component"
}

$expectedFfmpegDlls = @(
  "avcodec-63.dll",
  "avformat-63.dll",
  "avutil-61.dll",
  "swscale-10.dll"
) | Sort-Object
$actualFfmpegDlls = @(
  Get-ChildItem $root -Recurse -File |
    Where-Object Name -Match '^(?:avcodec|avformat|avutil|swscale|avfilter|avdevice|swresample)-\d+\.dll$' |
    ForEach-Object Name |
    Sort-Object
)
$ffmpegDiff = @(Compare-Object $expectedFfmpegDlls $actualFfmpegDlls)
if ($ffmpegDiff.Count -ne 0) {
  throw "Packaged FFmpeg DLL surface changed unexpectedly: $($ffmpegDiff | Out-String)"
}

$redist = @(Get-ChildItem $root -Recurse -File -Filter "VC_redist*.exe")
if ($redist.Count -ne 0) {
  throw "Portable package contains an unexpected Visual C++ Redistributable installer"
}
$appLocalCrt = @(
  Get-ChildItem $root -Recurse -File |
    Where-Object Name -Match '^(?:concrt|msvcp|vcruntime|msvcr)\d.*\.dll$'
)
if ($appLocalCrt.Count -ne 0) {
  throw "Portable package contains unexpected MSVC runtime DLLs"
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
  throw "Portable package still imports the dynamic MSVC runtime:$([Environment]::NewLine)$($runtimeImports -join [Environment]::NewLine)"
}

$gdupeImports = (& $dumpbin.FullName /nologo /dependents (Join-Path $root "gdupe.exe")) -join [Environment]::NewLine
foreach ($dll in $expectedFfmpegDlls) {
  if (-not $gdupeImports.Contains($dll)) {
    throw "gdupe.exe does not directly import required minimal FFmpeg DLL $dll"
  }
}

$archive = Join-Path $env:GITHUB_WORKSPACE "dist\gdupe-windows-x64.zip"
Compress-Archive -Path (Join-Path $root "*") -DestinationPath $archive
