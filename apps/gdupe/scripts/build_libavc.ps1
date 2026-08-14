$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

foreach ($name in @(
  "GDUPE_LIBAVC_DIR",
  "RUNNER_TEMP"
)) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "Required environment variable is missing: $name"
  }
}

$commit = "e67b0aa3b0f5eb15b40c4d148c5d070d8a2c6828"
$source = Join-Path $env:RUNNER_TEMP "gdupe-libavc-src"
$build = Join-Path $env:RUNNER_TEMP "gdupe-libavc-build"
$sdk = $env:GDUPE_LIBAVC_DIR

Remove-Item -Recurse -Force $source, $build -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $sdk | Out-Null

git clone --filter=blob:none https://android.googlesource.com/platform/external/libavc $source
git -C $source checkout $commit
if ((git -C $source rev-parse HEAD).Trim() -ne $commit) {
  throw "libavc checkout did not resolve to the pinned commit"
}

# AOSP libavc does not provide a supported Windows/MSVC target. Keep the
# adaptation strictly in compiler/OS glue; codec implementation remains at the
# pinned upstream commit.
$cmakeFiles = @(Get-ChildItem $source -Recurse -File -Include CMakeLists.txt,*.cmake)
$warningHits = @($cmakeFiles | Select-String -SimpleMatch '-Wdeclaration-after-statement')
if ($warningHits.Count -eq 0) {
  throw "Expected upstream GCC-only warning flag was not found"
}
foreach ($file in ($warningHits.Path | Sort-Object -Unique)) {
  $text = Get-Content $file -Raw
  $text = $text.Replace('-Wdeclaration-after-statement', '')
  Set-Content -Path $file -Value $text -Encoding utf8
}

Copy-Item apps/gdupe/experiments/libavc/ithread_windows.c `
  (Join-Path $source "common/ithread.c") -Force
Copy-Item apps/gdupe/experiments/libavc/ih264_platform_macros_msvc.h `
  (Join-Path $source "common/x86/ih264_platform_macros.h") -Force

cmake -S $source -B $build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
cmake --build $build --config Release --target libavcdec --parallel

$library = Join-Path $build "Release/libavcdec.lib"
if (-not (Test-Path -LiteralPath $library -PathType Leaf)) {
  throw "libavc static decoder library was not produced"
}
if (@(Get-ChildItem $build -Recurse -File -Filter *.dll).Count -ne 0) {
  throw "libavc unexpectedly produced a DLL"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = (& $vswhere -latest -products * -property installationPath).Trim()
$dumpbin = Get-ChildItem "$vsInstall\VC\Tools\MSVC" -Recurse -File -Filter dumpbin.exe |
  Where-Object FullName -Match '\\Hostx64\\x64\\dumpbin\.exe$' |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $dumpbin) { throw "dumpbin.exe was not found" }
$directives = (& $dumpbin.FullName /nologo /directives $library 2>&1 | Out-String)
if ($directives -notmatch '(?i)LIBCMT') { throw "libavc is not using the static MSVC CRT" }
if ($directives -match '(?i)MSVCRT') { throw "libavc requests the dynamic MSVC CRT" }

Remove-Item -Recurse -Force $sdk -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force `
  (Join-Path $sdk "lib"),
  (Join-Path $sdk "include/common"),
  (Join-Path $sdk "include/decoder"),
  (Join-Path $sdk "license") | Out-Null
Copy-Item $library (Join-Path $sdk "lib/libavcdec.lib")
Copy-Item (Join-Path $source "common/*.h") (Join-Path $sdk "include/common")
Copy-Item (Join-Path $source "common/x86/*.h") (Join-Path $sdk "include/common")
Copy-Item (Join-Path $source "decoder/*.h") (Join-Path $sdk "include/decoder")
Copy-Item (Join-Path $source "LICENSE") (Join-Path $sdk "license/LICENSE")

@"
AOSP libavc
Source: https://android.googlesource.com/platform/external/libavc
Pinned commit: $commit
License: Apache-2.0
Build: static libavcdec.lib, MSVC x64, /MT
Windows adaptation:
- removes upstream GCC-only -Wdeclaration-after-statement flag
- apps/gdupe/experiments/libavc/ithread_windows.c
- apps/gdupe/experiments/libavc/ih264_platform_macros_msvc.h
"@ | Set-Content -Encoding utf8 (Join-Path $sdk "SOURCE.txt")

Write-Host "Built static libavc SDK at $sdk"
