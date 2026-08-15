$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

foreach ($name in @("GDUPE_LIBHEVC_DIR", "RUNNER_TEMP")) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "Required environment variable is missing: $name"
  }
}

$commit = "1476a94a5ecbaed7a0aa37a7b26f1ee6d22a3dac"
$modificationDate = "2026-08-15"
$source = Join-Path $env:RUNNER_TEMP "gdupe-libhevc-src"
$build = Join-Path $env:RUNNER_TEMP "gdupe-libhevc-build"
$sdk = $env:GDUPE_LIBHEVC_DIR
Remove-Item -Recurse -Force $source, $build -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $sdk | Out-Null

git clone --filter=blob:none https://android.googlesource.com/platform/external/libhevc $source
git -C $source checkout $commit
if ((git -C $source rev-parse HEAD).Trim() -ne $commit) { throw "libhevc checkout did not resolve to the pinned commit" }

Copy-Item apps/gdupe/portability/libhevc/ithread_windows.c (Join-Path $source "common/ithread.c") -Force
Copy-Item apps/gdupe/portability/libhevc/ihevc_platform_macros_msvc.h (Join-Path $source "common/x86/ihevc_platform_macros.h") -Force
$utilsPath = Join-Path $source "cmake/utils.cmake"
$utils = Get-Content $utilsPath -Raw
$utils = $utils -replace 'link_libraries\(Threads::Threads m\)', 'link_libraries(Threads::Threads)'
if ($utils -match 'link_libraries\(Threads::Threads m\)') { throw "Failed to remove Unix libm linkage from libhevc" }
$marker = "# Modified for gdupe on ${modificationDate}: removed Unix libm linkage for the MSVC build.`r`n"
Set-Content -Path $utilsPath -Value ($marker + $utils) -Encoding utf8

cmake -S $source -B $build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW `
  -DCMAKE_C_FLAGS="/DX86_MSVC /DWINDOWS_TIMER"
cmake --build $build --config Release --target libhevcdec --parallel

$library = Join-Path $build "Release/libhevcdec.lib"
if (-not (Test-Path -LiteralPath $library -PathType Leaf)) { throw "libhevc static decoder library was not produced" }
if (@(Get-ChildItem $build -Recurse -File -Filter *.dll).Count -ne 0) { throw "libhevc unexpectedly produced a DLL" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = (& $vswhere -latest -products * -property installationPath).Trim()
$dumpbin = Get-ChildItem "$vsInstall\VC\Tools\MSVC" -Recurse -File -Filter dumpbin.exe |
  Where-Object FullName -Match '\\Hostx64\\x64\\dumpbin\.exe$' | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $dumpbin) { throw "dumpbin.exe was not found" }
$directives = (& $dumpbin.FullName /nologo /directives $library 2>&1 | Out-String)
if ($directives -match '(?i)\bLIBCMTD\b') { throw "libhevc requests the debug static MSVC CRT" }
if ($directives -notmatch '(?i)\bLIBCMT\b') { throw "libhevc is not using the release static MSVC CRT" }
if ($directives -match '(?i)\b(?:MSVCRT|UCRTBASE)\b') { throw "libhevc requests a dynamic MSVC/UCRT runtime" }

Remove-Item -Recurse -Force $sdk -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $sdk "lib"),(Join-Path $sdk "include/common"),(Join-Path $sdk "include/decoder"),(Join-Path $sdk "license") | Out-Null
Copy-Item $library (Join-Path $sdk "lib/libhevcdec.lib")
Copy-Item (Join-Path $source "common/*.h") (Join-Path $sdk "include/common")
Copy-Item (Join-Path $source "common/x86/*.h") (Join-Path $sdk "include/common")
Copy-Item (Join-Path $source "decoder/*.h") (Join-Path $sdk "include/decoder")
Copy-Item (Join-Path $source "LICENSE") (Join-Path $sdk "license/LICENSE")
Copy-Item (Join-Path $source "NOTICE") (Join-Path $sdk "license/NOTICE")
@"
AOSP libhevc
Source: https://android.googlesource.com/platform/external/libhevc
Pinned commit: $commit
License: Apache-2.0
Build: static libhevcdec.lib, MSVC x64, release /MT (LIBCMT)
Windows adaptation:
- apps/gdupe/portability/libhevc/ithread_windows.c
- apps/gdupe/portability/libhevc/ihevc_platform_macros_msvc.h
- removes Unix libm from cmake/utils.cmake for the Windows build
Modified-source notices are carried in the changed files. The upstream LICENSE and NOTICE are distributed with gdupe.
"@ | Set-Content -Encoding utf8 (Join-Path $sdk "SOURCE.txt")
Write-Host "Built static release-/MT libhevc SDK at $sdk"
