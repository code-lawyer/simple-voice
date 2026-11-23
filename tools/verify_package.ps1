param([Parameter(Mandatory=$true)][string]$Root,[switch]$RequireSigned,[switch]$ForUninstall)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
$manifest = Get-Content -LiteralPath (Join-Path $Root 'package-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.product -ne 'Simple Voice' -or $manifest.platform -ne 'windows-x64') { throw 'Unexpected package identity.' }
$seen = @{}
$checked = @{}
$binaries = @('voiceime_engine.exe','voiceime_model_worker.exe','voiceime_tsf.dll','voiceime_register.exe')
$uninstallFiles = @('voiceime_register.exe','libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll','tools/setup.ps1','tools/verify_package.ps1')
foreach ($entry in $manifest.files) {
  if ([IO.Path]::IsPathRooted($entry.path) -or $entry.path.Contains(':')) { throw 'Invalid manifest path.' }
  $file = [IO.Path]::GetFullPath((Join-Path $Root $entry.path))
  if (-not $file.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) -or $seen.ContainsKey($file)) { throw 'Unsafe or duplicate manifest path.' }
  $seen[$file] = $true
  if ($ForUninstall -and $entry.path -notin $uninstallFiles -and
      ($entry.path -notin $binaries -or -not (Test-Path -LiteralPath $file))) { continue }
  $info = Get-Item -LiteralPath $file -ErrorAction Stop
  if ($info.PSIsContainer -or ($info.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $info.Length -ne $entry.size -or
      (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Package integrity check failed: $($entry.path)" }
  $checked[$entry.path] = $true
}
$required = if ($ForUninstall) { $uninstallFiles } else { $binaries }
foreach ($name in $required) {
  if (-not $checked.ContainsKey($name)) { throw "Required package file missing from manifest: $name" }
}
$publisher = $null
foreach ($name in $binaries) {
  if ($ForUninstall -and -not (Test-Path -LiteralPath (Join-Path $Root $name))) { continue }
  $file = Join-Path $Root $name
  if (-not $seen.ContainsKey($file)) { throw "Required binary missing from manifest: $name" }
  $signature = Get-AuthenticodeSignature -LiteralPath $file
  if ($RequireSigned -or $manifest.version -notmatch '-rc\.') {
    if ($signature.Status -ne 'Valid') { throw "A public stable release requires a valid publisher signature: $name" }
    if ($publisher -and $publisher -ne $signature.SignerCertificate.Thumbprint) { throw 'Inconsistent package publisher.' }
    $publisher = $signature.SignerCertificate.Thumbprint
  }
}
Write-Host "Verified $($checked.Count) package files. Hashes detect corruption; publisher trust requires a valid signature."
