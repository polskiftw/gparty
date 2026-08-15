$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$tests = @(
  @{ Name = 'H.264/AVC'; File = 'gdupe_tests.exe' },
  @{ Name = 'HEVC Main'; File = 'gdupe_static_media_tests.exe' },
  @{ Name = 'HEVC Main 10'; File = 'gdupe_main10_tests.exe' },
  @{ Name = 'VP8 / VP9 / AV1'; File = 'gdupe_nvdec_webm_tests.exe' }
)

foreach ($test in $tests) {
  Write-Host "=== $($test.Name) ==="
  $executable = Join-Path $PSScriptRoot $test.File
  if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Missing test executable: $($test.File)"
  }
  & $executable
  $code = $LASTEXITCODE
  if ($code -eq 77) {
    throw "NVIDIA NVDEC is unavailable; verify the NVIDIA display driver is installed and the GPU is visible."
  }
  if ($code -ne 0) {
    throw "$($test.Name) regression failed with exit code $code."
  }
}

Write-Host ''
Write-Host 'ALL NVIDIA VIDEO REGRESSIONS PASSED'
Write-Host 'H.264, HEVC Main, HEVC Main 10, VP8, VP9, and AV1 decoded through gdupe NVDEC.'
