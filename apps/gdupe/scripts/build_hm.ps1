$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

foreach ($name in @(
  "GDUPE_HM_DIR",
  "RUNNER_TEMP"
)) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "Required environment variable is missing: $name"
  }
}

$commit = "fb4486d5ab5d0cd3b6a71659c7d5eb4509f2a4ce" # HM-18.0
$project = "jvet%2FHM"
$archiveUrl = "https://vcgit.hhi.fraunhofer.de/api/v4/projects/$project/repository/archive.zip?sha=$commit"
$archive = Join-Path $env:RUNNER_TEMP "gdupe-hm-$commit.zip"
$extract = Join-Path $env:RUNNER_TEMP "gdupe-hm-extract"
$source = Join-Path $env:RUNNER_TEMP "gdupe-hm-src"
$build = Join-Path $env:RUNNER_TEMP "gdupe-hm-build"
$sdk = $env:GDUPE_HM_DIR

Remove-Item -Recurse -Force $extract, $source, $build, $sdk -ErrorAction SilentlyContinue
Remove-Item -Force $archive -ErrorAction SilentlyContinue

for ($attempt = 1; $attempt -le 3; ++$attempt) {
  try {
    Invoke-WebRequest -Uri $archiveUrl -OutFile $archive
    if ((Get-Item $archive).Length -lt 100000) {
      throw "HM source archive is unexpectedly small"
    }
    break
  } catch {
    Remove-Item -Force $archive -ErrorAction SilentlyContinue
    if ($attempt -eq 3) { throw }
    Start-Sleep -Seconds (5 * $attempt)
  }
}

Expand-Archive -Path $archive -DestinationPath $extract -Force
$roots = @(Get-ChildItem $extract -Directory)
if ($roots.Count -ne 1) {
  throw "HM source archive did not contain exactly one source root"
}
Move-Item $roots[0].FullName $source
if (-not (Test-Path (Join-Path $source "CMakeLists.txt") -PathType Leaf)) {
  throw "HM source archive is missing CMakeLists.txt"
}

cmake -S $source -B $build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
cmake --build $build --config Release --target TLibCommon TLibDecoder --parallel

$common = Get-ChildItem $source -Recurse -File -Filter TLibCommon.lib | Select-Object -First 1
$decoder = Get-ChildItem $source -Recurse -File -Filter TLibDecoder.lib | Select-Object -First 1
if (-not $common -or -not $decoder) {
  throw "HM static decoder libraries were not produced"
}
if (@(Get-ChildItem $source -Recurse -File -Filter *.dll).Count -ne 0) {
  throw "HM unexpectedly produced a DLL"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = (& $vswhere -latest -products * -property installationPath).Trim()
$dumpbin = Get-ChildItem "$vsInstall\VC\Tools\MSVC" -Recurse -File -Filter dumpbin.exe |
  Where-Object FullName -Match '\\Hostx64\\x64\\dumpbin\.exe$' |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $dumpbin) { throw "dumpbin.exe was not found" }

foreach ($library in @($common, $decoder)) {
  $directives = (& $dumpbin.FullName /nologo /directives $library.FullName 2>&1 | Out-String)
  if ($directives -notmatch '(?i)LIBCMT') {
    throw "$($library.Name) is not using the static MSVC CRT"
  }
  if ($directives -match '(?i)MSVCRT') {
    throw "$($library.Name) requests the dynamic MSVC CRT"
  }
}

New-Item -ItemType Directory -Force `
  (Join-Path $sdk "lib"),
  (Join-Path $sdk "include/TLibCommon"),
  (Join-Path $sdk "include/TLibDecoder"),
  (Join-Path $sdk "include/libmd5"),
  (Join-Path $sdk "license") | Out-Null

Copy-Item $common.FullName (Join-Path $sdk "lib/TLibCommon.lib")
Copy-Item $decoder.FullName (Join-Path $sdk "lib/TLibDecoder.lib")
Copy-Item (Join-Path $source "source/Lib/TLibCommon/*.h") (Join-Path $sdk "include/TLibCommon")
Copy-Item (Join-Path $source "source/Lib/TLibDecoder/*.h") (Join-Path $sdk "include/TLibDecoder")
if (Test-Path (Join-Path $source "source/Lib/libmd5")) {
  Copy-Item (Join-Path $source "source/Lib/libmd5/*.h") (Join-Path $sdk "include/libmd5")
}

$license = @(
  (Join-Path $source "COPYING"),
  (Join-Path $source "LICENSE"),
  (Join-Path $source "LICENSE.txt")
) | Where-Object { Test-Path $_ -PathType Leaf } | Select-Object -First 1
if (-not $license) {
  throw "Could not locate HM license file in the pinned source archive"
}
Copy-Item $license (Join-Path $sdk "license/LICENSE")

@"
HEVC HM reference software
Source: https://vcgit.hhi.fraunhofer.de/jvet/HM
Pinned tag: HM-18.0
Pinned commit: $commit
License: BSD 3-Clause (retain the copied upstream license text)
Build: static TLibCommon.lib + TLibDecoder.lib, MSVC x64, /MT
Purpose in gdupe: HEVC/Main/Main10 decode only; no HM executable is shipped.
"@ | Set-Content -Encoding utf8 (Join-Path $sdk "SOURCE.txt")

Write-Host "Built static HM decoder SDK at $sdk"
