$ErrorActionPreference = "Stop"

$buildRoot = Join-Path $env:GITHUB_WORKSPACE "build\gdupe"
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
  "libpng",
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

$forbidden = @($records.Package | Where-Object {
  $_ -eq "opencv4" -or $_ -eq "fltk" -or $_.StartsWith("qt")
})
if ($forbidden.Count -ne 0) {
  throw "A forbidden dynamic/oversized UI dependency is present: $($forbidden -join ', ')"
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
Assert-Features "libpng" @("core")
Assert-Features "libwebp" @("core", "unicode")
Assert-Features "nlohmann-json" @("core")
Assert-Features "sqlite3" @("core")
Assert-Features "zlib" @("core")

$manifest = Get-Content (Join-Path $env:GITHUB_WORKSPACE "apps\gdupe\vcpkg.json") -Raw |
  ConvertFrom-Json
$declared = @($manifest.dependencies | ForEach-Object {
  if ($_ -is [string]) { $_ } else { $_.name }
} | Sort-Object)
$expectedDeclared = @(
  "curl", "libjpeg-turbo", "libpng", "libwebp", "nlohmann-json",
  "sqlite3"
) | Sort-Object
if (@(Compare-Object $expectedDeclared $declared).Count -ne 0) {
  throw "Top-level dependency manifest changed unexpectedly"
}

$cache = Get-Content (Join-Path $buildRoot "CMakeCache.txt") -Raw
$requiredCache = @{
  "FLTK_BUILD_SHARED_LIBS" = "OFF"
  "FLTK_BUILD_FORMS" = "OFF"
  "FLTK_BUILD_FLUID" = "OFF"
  "FLTK_BUILD_FLTK_OPTIONS" = "OFF"
  "FLTK_BUILD_EXAMPLES" = "OFF"
  "FLTK_BUILD_TEST" = "OFF"
  "FLTK_BUILD_GL" = "OFF"
  "FLTK_BUILD_HTML_DOCS" = "OFF"
  "FLTK_BUILD_PDF_DOCS" = "OFF"
  "FLTK_BUILD_FLUID_DOCS" = "OFF"
  "FLTK_INSTALL_HTML_DOCS" = "OFF"
  "FLTK_INSTALL_PDF_DOCS" = "OFF"
  "FLTK_INSTALL_FLUID_DOCS" = "OFF"
  "FLTK_INSTALL_LINKS" = "OFF"
  "FLTK_GRAPHICS_GDIPLUS" = "OFF"
  "FLTK_OPTION_CAIRO_EXT" = "OFF"
  "FLTK_OPTION_CAIRO_WINDOW" = "OFF"
  "FLTK_OPTION_PRINT_SUPPORT" = "OFF"
  "FLTK_OPTION_FILESYSTEM_SUPPORT" = "OFF"
  "FLTK_OPTION_LARGE_FILE" = "OFF"
  "FLTK_OPTION_SVG" = "OFF"
  "FLTK_USE_SYSTEM_LIBJPEG" = "ON"
  "FLTK_USE_SYSTEM_LIBPNG" = "ON"
  "FLTK_USE_SYSTEM_ZLIB" = "ON"
}
foreach ($entry in $requiredCache.GetEnumerator()) {
  if ($cache -notmatch "(?m)^$([regex]::Escape($entry.Key)):[^=]+=$([regex]::Escape($entry.Value))$") {
    throw "FLTK cache invariant failed: $($entry.Key) must be $($entry.Value)"
  }
}

$triplet = Get-Content (Join-Path $env:GITHUB_WORKSPACE "apps\gdupe\triplets\x64-windows-static-crt.cmake") -Raw
foreach ($setting in @("VCPKG_CRT_LINKAGE static", "VCPKG_LIBRARY_LINKAGE static")) {
  if (-not $triplet.Contains("set($setting)")) {
    throw "Static triplet invariant failed: set($setting)"
  }
}

Write-Host "Exact static dependency surface verified; Qt and OpenCV are absent."
