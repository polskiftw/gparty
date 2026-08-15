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
  "dav1d",
  "libjpeg-turbo",
  "libpng",
  "libvpx",
  "libwebm",
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
Assert-Features "dav1d" @("core")
Assert-Features "libjpeg-turbo" @("core")
Assert-Features "libpng" @("core")
Assert-Features "libvpx" @("core", "highbitdepth")
Assert-Features "libwebm" @("core")
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
  "curl", "dav1d", "libjpeg-turbo", "libpng", "libvpx", "libwebm",
  "libwebp", "nlohmann-json", "sqlite3"
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
  if ($cache -notmatch "(?m)^$([regex]::Escape($entry.Key)):[^=]+=$([regex]::Escape($entry.Value))\r?$") {
    throw "FLTK cache invariant failed: $($entry.Key) must be $($entry.Value)"
  }
}

# FLTK's own feature options do not reliably prune its Windows source list.
# Audit the generated projects so the exact gdupe-only source surgery cannot
# silently regress when CMake logic changes.
$fltkProject = Get-ChildItem -LiteralPath $buildRoot -Recurse -Filter "fltk.vcxproj" -File |
  Select-Object -First 1
$fltkImagesProject = Get-ChildItem -LiteralPath $buildRoot -Recurse -Filter "fltk_images.vcxproj" -File |
  Select-Object -First 1
if (-not $fltkProject -or -not $fltkImagesProject) {
  throw "Generated FLTK project files were not found"
}
$fltkProjectText = Get-Content -LiteralPath $fltkProject.FullName -Raw
$fltkImagesProjectText = Get-Content -LiteralPath $fltkImagesProject.FullName -Raw

$forbiddenFltkSources = @(
  "Fl_Printer.cxx",
  "Fl_Paged_Device.cxx",
  "Fl_WinAPI_Printer_Driver.cxx",
  "Fl_PostScript.cxx",
  "Fl_PostScript_image.cxx",
  "print_button.cxx",
  "Fl_File_Chooser.cxx",
  "Fl_File_Chooser2.cxx",
  "Fl_Native_File_Chooser.cxx",
  "Fl_Native_File_Chooser_WIN32.cxx",
  "Fl_SVG_Image.cxx",
  "Fl_SVG_File_Surface.cxx",
  "Fl_JPEG_Image.cxx",
  "Fl_PNG_Image.cxx",
  "Fl_BMP_Image.cxx",
  "Fl_PNM_Image.cxx",
  "Fl_arg.cxx",
  "fl_ask.cxx",
  "Fl_Browser.cxx",
  "Fl_Browser_.cxx",
  "Fl_Input.cxx",
  "Fl_Input_.cxx",
  "Fl_Message.cxx",
  "Fl_Scrollbar.cxx",
  "Fl_Slider.cxx",
  "Fl_Text_Buffer.cxx",
  "Fl_Text_Display.cxx",
  "Fl_Text_Editor.cxx",
  "Fl_Valuator.cxx",
  "Fl_Window_hotspot.cxx",
  "fl_gleam.cxx",
  "fl_plastic.cxx",
  "Fl_Tiled_Image.cxx"
)
foreach ($source in $forbiddenFltkSources) {
  if ($fltkProjectText.Contains($source) -or $fltkImagesProjectText.Contains($source)) {
    throw "Forbidden FLTK source re-entered the gdupe build: $source"
  }
}

$imageCompileSources = @(
  [regex]::Matches($fltkImagesProjectText, '<ClCompile Include="[^"]*[\\/]([^\\/"]+\.cxx)"') |
    ForEach-Object { $_.Groups[1].Value } |
    Sort-Object -Unique
)
$expectedImageCompileSources = @(
  "Fl_Anim_GIF_Image.cxx",
  "Fl_GIF_Image.cxx",
  "Fl_Image_Reader.cxx"
) | Sort-Object
$imageDifference = @(Compare-Object $expectedImageCompileSources $imageCompileSources)
if ($imageDifference.Count -ne 0) {
  throw "FLTK image source surface changed unexpectedly: $($imageDifference | Out-String)"
}
if (-not $fltkProjectText.Contains("fltk_print_stub.cpp")) {
  throw "FLTK Win32 print bootstrap stub is missing from the generated core target"
}

$triplet = Get-Content (Join-Path $env:GITHUB_WORKSPACE "apps\gdupe\triplets\x64-windows-static-crt.cmake") -Raw
foreach ($setting in @("VCPKG_CRT_LINKAGE static", "VCPKG_LIBRARY_LINKAGE static")) {
  if (-not $triplet.Contains("set($setting)")) {
    throw "Static triplet invariant failed: set($setting)"
  }
}

Write-Host "Exact static dependency and FLTK source surfaces verified; Qt and OpenCV are absent."