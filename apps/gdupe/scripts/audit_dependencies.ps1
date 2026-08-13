$ErrorActionPreference = "Stop"

$statusPath = Join-Path $env:GITHUB_WORKSPACE "build\gdupe\vcpkg_installed\vcpkg\status"
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

if ($records.Package -contains "opencv4") {
  throw "OpenCV is still present in the target dependency graph"
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

Assert-Features "qtbase" @(
  "concurrent",
  "core",
  "doubleconversion",
  "future",
  "gui",
  "jpeg",
  "network",
  "png",
  "thread",
  "widgets",
  "windeployqt"
)
Assert-Features "qtimageformats" @("core", "webp")
Assert-Features "qtmultimedia" @("core", "widgets")

Write-Host "OpenCV absent; exact Qt target feature surface verified."
