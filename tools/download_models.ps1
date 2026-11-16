param(
  [string]$Root = (Split-Path -Parent $PSScriptRoot),
  [switch]$ForPackage,
  [switch]$Offline,
  [switch]$RuntimeOnly,
  [ValidateSet('qwen3','sensevoice','whisper','chinese-legacy')][string]$OnlyModel,
  [string]$StatusFile
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# A GUI child may inherit PSModulePath from PowerShell 7 or another host. Bind
# the inbox modules belonging to this PowerShell runtime before using cmdlets.
foreach ($module in @('Microsoft.PowerShell.Management','Microsoft.PowerShell.Utility')) {
  Import-Module (Join-Path $PSHOME "Modules/$module/$module.psd1") -ErrorAction Stop
}
$Root = [IO.Path]::GetFullPath($Root)
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json
if ($OnlyModel -and ($RuntimeOnly -or $ForPackage)) { throw 'A model-only download cannot replace runtime files.' }
if ($StatusFile) {
  $StatusFile = [IO.Path]::GetFullPath($StatusFile)
  if (-not $StatusFile.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Download status must stay inside the selected data directory.'
  }
}
function Set-DownloadStatus([string]$Text) {
  if ($StatusFile) { try { [IO.File]::WriteAllText($StatusFile, $Text, [Text.UTF8Encoding]::new($false)) } catch {} }
}
trap { Set-DownloadStatus ("下载失败，可重试。" + [Environment]::NewLine + $_.Exception.Message); Write-Error $_ -ErrorAction Continue; exit 1 }
$selected = $null
$lock = $null
if ($OnlyModel) {
  $selected = @{qwen3='final-qwen';sensevoice='final-sense';whisper='final-whisper';'chinese-legacy'='final'}[$OnlyModel]
  New-Item -ItemType Directory -Force -Path (Join-Path $Root '.downloads') | Out-Null
  $lock = [IO.File]::Open((Join-Path $Root '.downloads/model-download.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
}

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
  # A usable preset includes its shared draft model (and legacy punctuation).
  # Model-only downloads still never replace the running runtime binaries.
  if ($OnlyModel -and $pack.Id -ne $selected -and $pack.Id -ne 'streaming-bilingual' -and
      -not ($OnlyModel -eq 'chinese-legacy' -and $pack.Id -eq 'punct')) { continue }
  if (-not $OnlyModel -and $pack.PSObject.Properties['Optional'] -and $pack.Optional) { continue }
  if ($RuntimeOnly -and $pack.Id -ne 'runtime') { continue }
  Set-DownloadStatus '正在检查本地文件…'
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
      Set-DownloadStatus '正在下载模型。可取消；完成后会校验完整性。'
      $partial = $archive + '.partial'
      $completePartial = (Test-Path -LiteralPath $partial -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -eq $pack.Sha256)
      if (-not $completePartial) {
        & curl.exe --location --fail --retry 3 --connect-timeout 30 --max-time 1800 --continue-at - --output $partial $pack.Url
        if ($LASTEXITCODE -eq 33) {
          # Some HTTP endpoints cannot resume. Keep the old bytes recoverable.
          $previous = Get-ChildPath ('.downloads/unresumable-' + [Guid]::NewGuid().ToString('N') + '.tar.bz2')
          if (Test-Path -LiteralPath $partial) { Move-Item -LiteralPath $partial -Destination $previous }
          & curl.exe --location --fail --retry 3 --connect-timeout 30 --max-time 1800 --output $partial $pack.Url
        }
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $($pack.Url). Run setup again to retry." }
      }
      Set-DownloadStatus '下载完成，正在校验…'
      if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $pack.Sha256) {
        # Preserve corrupt downloads for diagnosis; a retry starts a new file.
        $quarantine = Get-ChildPath ('.downloads/invalid-' + [Guid]::NewGuid().ToString('N') + '.tar.bz2')
        Move-Item -LiteralPath $partial -Destination $quarantine
        throw "SHA-256 mismatch for $($pack.Id); nothing was installed."
      }
      Move-Item -LiteralPath $partial -Destination $archive -Force
    }
    $stage = Get-ChildPath ('.downloads/extract-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    Set-DownloadStatus '正在解压并校验模型文件…'
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

if ($lock) { $lock.Dispose() }
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
Set-DownloadStatus '模型文件已下载并通过完整性校验。'
