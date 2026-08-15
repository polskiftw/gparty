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
  "dav1d",
  "libjpeg-turbo",
  "libvpx",
  "libwebm",
  "libwebp",
  "nlohmann-json",
  "sqlite3"
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
Assert-Features "dav1d" @("core")
Assert-Features "libjpeg-turbo" @("core")
Assert-Features "libvpx" @("core", "highbitdepth")
Assert-Features "libwebm" @("core")
Assert-Features "libwebp" @("core", "unicode")
Assert-Features "nlohmann-json" @("core")
Assert-Features "sqlite3" @("core")

$manifest = Get-Content (Join-Path $appRoot "vcpkg.json") -Raw | ConvertFrom-Json
$declared = @($manifest.dependencies | ForEach-Object {
  if ($_ -is [string]) { $_ } else { $_.name }
} | Sort-Object)
$expectedDeclared = @(
  "curl", "dav1d", "libjpeg-turbo", "libvpx", "libwebm", "libwebp",
  "nlohmann-json", "sqlite3"
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
  'src/wic_gif.cpp',
  'd2d1',
  'dwrite',
  'windowscodecs',
  'GIT_TAG 5a212a18dba7dca09543bbc7d65619274fd2931a'
)) {
  if (-not $cmake.Contains($required)) {
    throw "Native Windows build invariant is missing: $required"
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
  'nanosvg'
)) {
  if ($cmake.Contains($forbiddenSnippet)) {
    throw "Retired dependency residue found in CMake: $forbiddenSnippet"
  }
}

$sourceFiles = Get-ChildItem (Join-Path $appRoot "src") -Recurse -File -Include *.cpp,*.hpp
$sourceResidue = @($sourceFiles | Select-String -Pattern '(?i)(?:<FL/|\bFLTK\b|NanoSVG|nanosvg|opencv|libav(?:codec|format|util)|<libav)')
if ($sourceResidue.Count -ne 0) {
  throw "Retired dependency residue found in production source: $($sourceResidue | Out-String)"
}

$generatedProjects = @(Get-ChildItem $buildRoot -File -Filter *.vcxproj)
$generatedResidue = @($generatedProjects | Select-String -Pattern '(?i)fltk|nanosvg|opencv|libpng')
if ($generatedResidue.Count -ne 0) {
  throw "Retired dependency found in generated Visual Studio projects: $($generatedResidue | Out-String)"
}

foreach ($sdk in @("LIBAVC", "LIBHEVC")) {
  $path = (Get-Item "env:GDUPE_${sdk}_DIR").Value
  if (-not (Test-Path -LiteralPath (Join-Path $path "lib\$($sdk.ToLower())dec.lib") -PathType Leaf) -or
      -not (Test-Path -LiteralPath (Join-Path $path "SOURCE.txt") -PathType Leaf)) {
    throw "Source-pinned AOSP $sdk SDK boundary is incomplete"
  }
}

Write-Host "Exact static dependency surface verified: native Windows UI, WIC PNG/GIF, and no retired GUI/media stack."
