$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$build = Join-Path $root 'build\gdupe'
$report = Join-Path $root 'gdupe-crt-diagnostics.txt'

$lines = [System.Collections.Generic.List[string]]::new()
function Add-Line([string]$text = '') {
  $lines.Add($text)
  Write-Host $text
}

Add-Line 'gdupe release CRT provenance diagnostics'
Add-Line ('generated: ' + (Get-Date -Format o))
Add-Line ''

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
  foreach ($pattern in @('AdditionalDependencies', 'AdditionalLibraryDirectories', 'RuntimeLibrary')) {
    $matches = $xml | Select-String -SimpleMatch $pattern
    foreach ($match in $matches) {
      Add-Line ($match.Line.Trim())
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
  $output = & dumpbin.exe /nologo /directives $file.FullName 2>&1 | Out-String
  if ($output -match '(?i)LIBCMTD') {
    $culprits.Add($file.FullName)
    Add-Line ('DEBUG CRT DIRECTIVE: ' + $file.FullName)
    foreach ($line in ($output -split "`r?`n" | Where-Object { $_ -match '(?i)LIBCMTD' })) {
      Add-Line ('  ' + $line.Trim())
    }
  }
}

Add-Line ''
Add-Line ('LIBCMTD directive files: ' + $culprits.Count)
foreach ($culprit in $culprits) { Add-Line ('  ' + $culprit) }

$buildLog = Join-Path $root 'gdupe-msvc-build.log'
if (Test-Path $buildLog) {
  $warningLines = Get-Content $buildLog | Where-Object { $_ -match '(?i)LNK4098|LIBCMTD' }
  Add-Line ''
  Add-Line ('link warning lines: ' + $warningLines.Count)
  foreach ($warning in $warningLines) { Add-Line ('  ' + $warning) }
}

$lines | Set-Content -Encoding utf8 $report
Write-Host "CRT diagnostics written to $report"
