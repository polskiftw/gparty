$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$build = Join-Path $root 'build\gdupe'
$report = Join-Path $root 'gdupe-crt-diagnostics.txt'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = (& $vswhere -latest -products * -property installationPath).Trim()
$dumpbin = Get-ChildItem "$vsInstall\VC\Tools\MSVC" -Recurse -File -Filter dumpbin.exe |
  Where-Object FullName -Match '\\Hostx64\\x64\\dumpbin\.exe$' |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $dumpbin) {
  throw 'dumpbin.exe was not found'
}

$lines = [System.Collections.Generic.List[string]]::new()
function Add-Line([string]$text = '') {
  $lines.Add($text)
  Write-Host $text
}

Add-Line 'gdupe release CRT provenance diagnostics'
Add-Line ('generated: ' + (Get-Date -Format o))
Add-Line ('dumpbin: ' + $dumpbin.FullName)
Add-Line ''

# FLTK 1.4.5 uses FindJPEG/FindPNG/FindZLIB list variables for its image target.
# With a multi-config Visual Studio generator those lists can leave debug-path
# alternatives in AdditionalDependencies even for Release. They are harmless if
# no object from those alternatives is selected. Record them for visibility, but
# judge the Release build by actual CRT directives and linker diagnostics.
$releaseDebugMetadata = [System.Collections.Generic.List[string]]::new()
$projects = @(
  (Join-Path $build 'gdupe.vcxproj'),
  (Join-Path $build 'gdupe_tests.vcxproj'),
  (Join-Path $build 'gdupe_static_media_tests.vcxproj')
)
foreach ($project in $projects) {
  Add-Line ('=== project: ' + $project + ' ===')
  if (-not (Test-Path $project)) {
    Add-Line 'missing'
    Add-Line ''
    continue
  }
  $xml = Get-Content $project
  foreach ($pattern in @('AdditionalDependencies', 'AdditionalLibraryDirectories', 'RuntimeLibrary', 'AdditionalOptions')) {
    $matches = $xml | Select-String -SimpleMatch $pattern
    foreach ($match in $matches) {
      $text = $match.Line.Trim()
      Add-Line $text
      if ($pattern -eq 'AdditionalDependencies' -and
          $text -match '<AdditionalDependencies>Release\\' -and
          $text -match '(?i)[\\/]debug[\\/]') {
        $releaseDebugMetadata.Add("$project :: $text")
      }
    }
  }
  Add-Line ''
}

$scanRoots = @(
  (Join-Path $build 'Release'),
  (Join-Path $build '_deps\fltk-build\lib\Release'),
  (Join-Path $build 'vcpkg_installed\x64-windows-static-crt\lib'),
  (Join-Path $root 'third_party\libavc\lib'),
  (Join-Path $root 'third_party\libhevc\lib')
)

$files = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
foreach ($scanRoot in $scanRoots) {
  if (Test-Path $scanRoot) {
    Get-ChildItem $scanRoot -File -Recurse | Where-Object {
      $_.Extension -in @('.lib', '.obj') -and $_.FullName -notmatch '[\\/]debug[\\/]'
    } | ForEach-Object { $files.Add($_) }
  }
}

# Source objects can request a CRT even when the surrounding .lib does not make
# the origin obvious, so scan the Release object directories as well.
Get-ChildItem $build -File -Recurse -Filter '*.obj' | Where-Object {
  $_.FullName -match '[\\/]Release[\\/]' -and $_.FullName -notmatch '[\\/]debug[\\/]'
} | ForEach-Object { $files.Add($_) }

$unique = $files | Sort-Object FullName -Unique
Add-Line ('Scanning ' + $unique.Count + ' Release .lib/.obj files for LIBCMTD directives...')
$culprits = [System.Collections.Generic.List[string]]::new()
foreach ($file in $unique) {
  $output = & $dumpbin.FullName /nologo /directives $file.FullName 2>&1 | Out-String
  if ($output -match '(?i)LIBCMTD') {
    $culprits.Add($file.FullName)
    Add-Line ('DEBUG CRT DIRECTIVE: ' + $file.FullName)
    foreach ($line in ($output -split "`r?`n" | Where-Object { $_ -match '(?i)LIBCMTD' })) {
      Add-Line ('  ' + $line.Trim())
    }
  }
}

Add-Line ''
Add-Line ('Release dependency metadata lines mentioning debug/: ' + $releaseDebugMetadata.Count)
foreach ($entry in $releaseDebugMetadata) { Add-Line ('  advisory: ' + $entry) }
Add-Line ('LIBCMTD directive files: ' + $culprits.Count)
foreach ($culprit in $culprits) { Add-Line ('  ' + $culprit) }

# LNK4098 is the actual MSVC runtime-conflict warning. /NODEFAULTLIB:LIBCMTD is
# deliberately applied to every Release executable as an additional hard stop.
$warningLines = @()
$buildLog = Join-Path $root 'gdupe-msvc-build.log'
if (Test-Path $buildLog) {
  $warningLines = @(Get-Content $buildLog | Where-Object { $_ -match '(?i)LNK4098|LNK2038.*RuntimeLibrary' })
  Add-Line ''
  Add-Line ('runtime conflict lines: ' + $warningLines.Count)
  foreach ($warning in $warningLines) { Add-Line ('  ' + $warning) }
}

$lines | Set-Content -Encoding utf8 $report
Write-Host "CRT diagnostics written to $report"

if ($culprits.Count -ne 0) {
  throw 'A Release library/object requests the debug MSVC CRT (LIBCMTD).'
}
if ($warningLines.Count -ne 0) {
  throw 'The Release linker emitted a debug-CRT conflict diagnostic.'
}

Write-Host 'Release CRT audit passed: /MT only, no linked debug-CRT provenance found.'
