param(
  [string]$Root = (Split-Path -Parent $PSScriptRoot),
  [switch]$ForPackage,
  [switch]$Offline,
  [switch]$RuntimeOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = [IO.Path]::GetFullPath($Root)
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json

function Get-ChildPath([string]$Relative) {
  $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
  if (-not $path.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Dependency path must be inside the installation directory: $Relative"
  }
  return $path
}

function Test-Files([string]$Directory, $Files) {
  foreach ($file in $Files.PSObject.Properties) {
    $path = Join-Path $Directory $file.Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.Value) { return $false }
  }
  return $true
}

foreach ($pack in $manifest.artifacts) {
  if ($RuntimeOnly -and $pack.Id -ne 'runtime') { continue }
  $destination = Get-ChildPath $pack.Destination
  if (-not (Test-Files $destination $pack.Files)) {
    $cache = Get-ChildPath '.downloads'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $archive = Join-Path $cache ($pack.Folder + '.tar.bz2')
    $validArchive = (Test-Path -LiteralPath $archive -PathType Leaf) -and
      ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -eq $pack.Sha256)
    if (-not $validArchive) {
      if ($Offline) { throw "Verified cache is missing for $($pack.Id). Run setup once with an internet connection." }
      Write-Host "Downloading $($pack.Id) from the official sherpa-onnx release..."
      $partial = $archive + '.partial'
      & curl.exe --location --fail --retry 3 --connect-timeout 30 --output $partial $pack.Url
      if ($LASTEXITCODE -ne 0) { throw "Download failed: $($pack.Url). Run setup again to retry." }
      if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $pack.Sha256) {
        throw "SHA-256 mismatch for $($pack.Id); nothing was installed."
      }
      Move-Item -LiteralPath $partial -Destination $archive -Force
    }
    $stage = Get-ChildPath ('.downloads/extract-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    & tar.exe -xjf $archive -C $stage
    if ($LASTEXITCODE -ne 0) { throw "Cannot extract $archive" }
    $extracted = Join-Path $stage $pack.Folder
    if (-not (Test-Files $extracted $pack.Files)) { throw "Extracted files failed verification: $($pack.Id)" }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    # Keep an existing incomplete/older directory recoverable. Never delete user data.
    $backup = $null
    if (Test-Path -LiteralPath $destination) {
      $backup = Get-ChildPath ($pack.Destination + '.previous-' + [Guid]::NewGuid().ToString('N'))
      Move-Item -LiteralPath $destination -Destination $backup
    }
    try { Move-Item -LiteralPath $extracted -Destination $destination }
    catch {
      if ($backup -and -not (Test-Path -LiteralPath $destination)) {
        Move-Item -LiteralPath $backup -Destination $destination
      }
      throw
    }
  }
  Write-Host "Verified: $($pack.Id)"
}

if ($ForPackage) {
  $runtime = $manifest.artifacts | Where-Object Id -eq 'runtime'
  foreach ($name in @('sherpa-onnx-c-api.dll', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll')) {
    $target = Join-Path $Root $name
    $expected = $runtime.Files.PSObject.Properties['lib/' + $name].Value
    if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $expected) {
      Copy-Item -LiteralPath (Join-Path (Get-ChildPath $runtime.Destination) ('lib/' + $name)) -Destination $target -Force
    }
  }
}
if ($RuntimeOnly) { Write-Host 'Verified runtime SDK is ready for building.' }
else { Write-Host 'Local models are ready. Normal input does not require a network connection.' }
