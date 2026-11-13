$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$third = Join-Path $root "third_party\sherpa-onnx"
$models = Join-Path $root "models"
New-Item -ItemType Directory -Force -Path $third, $models | Out-Null

function Get-Archive($url, $dest) {
  if (Test-Path $dest) { return }
  Write-Output "download $url"
  curl.exe -L --fail --retry 3 -o $dest $url
  if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
}

function Expand-Bz2($archive, $dest) {
  New-Item -ItemType Directory -Force -Path $dest | Out-Null
  tar.exe -xjf $archive -C $dest
  if ($LASTEXITCODE -ne 0) { throw "extract failed: $archive" }
}

$runtimeUrl = "https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.1/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release.tar.bz2"
$runtimeArchive = Join-Path $third "sherpa-onnx-v2.0.1-win-x64-shared-MD-Release.tar.bz2"
$runtimeDir = Join-Path $third "sherpa-onnx-v2.0.1-win-x64-shared-MD-Release"
if (-not (Test-Path (Join-Path $runtimeDir "include"))) {
  Get-Archive $runtimeUrl $runtimeArchive
  Expand-Bz2 $runtimeArchive $third
}

$packs = @(
  @{
    Name = "streaming"
    Url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-ctc-zh-int8-2025-06-30.tar.bz2"
    Folder = "sherpa-onnx-streaming-zipformer-ctc-zh-int8-2025-06-30"
  },
  @{
    Name = "final"
    Url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-zipformer-ctc-zh-int8-2025-07-03.tar.bz2"
    Folder = "sherpa-onnx-zipformer-ctc-zh-int8-2025-07-03"
  },
  @{
    Name = "punct"
    Url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/punctuation-models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8.tar.bz2"
    Folder = "sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8"
  }
)

foreach ($pack in $packs) {
  $dest = Join-Path $models $pack.Name
  $marker = Join-Path $dest ".complete"
  if (-not (Test-Path $marker)) {
    $archive = Join-Path $models ($pack.Folder + ".tar.bz2")
    Get-Archive $pack.Url $archive
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Expand-Bz2 $archive $models
    $extracted = Join-Path $models $pack.Folder
    if (Test-Path $extracted) {
      if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
      Rename-Item $extracted $dest
    }
    Remove-Item $archive -Force
    Set-Content -Path $marker -Value $pack.Folder
  }
  Write-Output ("{0}:" -f $pack.Name)
  Get-ChildItem $dest -Filter *.onnx | ForEach-Object { Write-Output ("  " + $_.Name) }
}
