param(
  [string]$BuildDir = 'build-release',
  [string]$OutputDir = 'dist',
  [switch]$RequireSigned
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$build = [IO.Path]::GetFullPath((Join-Path $source $BuildDir))
$output = [IO.Path]::GetFullPath((Join-Path $source $OutputDir))
$productHeader = Get-Content -LiteralPath (Join-Path $source 'src/product.hpp') -Raw
if ($productHeader -notmatch '#define SIMPLE_VOICE_VERSION "([0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?)"') { throw 'Missing package version.' }
$version = $Matches[1]
$name = "SimpleVoice-v$version-windows-x64"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$stage = Join-Path $output ('.stage-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $stage $name
New-Item -ItemType Directory -Path $package | Out-Null

function Copy-PackageFile([string]$From, [string]$Relative) {
  if (-not (Test-Path -LiteralPath $From -PathType Leaf)) { throw "Missing package input: $From" }
  $to = [IO.Path]::GetFullPath((Join-Path $package $Relative))
  if (-not $to.StartsWith($package + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid package destination.' }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
  Copy-Item -LiteralPath $From -Destination $to
}

foreach ($file in @('voiceime_engine.exe','voiceime_model_worker.exe','voiceime_register.exe','voiceime_tsf.dll')) {
  $path = Join-Path $build $file
  $info = (Get-Item -LiteralPath $path).VersionInfo
  if ($info.ProductName -ne 'Simple Voice' -or $info.FileVersion -ne $version) { throw "Unexpected product/version: $file" }
  Copy-PackageFile $path $file
}
foreach ($file in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-PackageFile (Join-Path $build $file) $file
}
foreach ($line in Get-Content -LiteralPath (Join-Path $source 'rime-data/SHA256SUMS')) {
  if ($line -notmatch '^([0-9a-f]{64})  ([A-Za-z0-9_.-]+)$') { throw 'Invalid Rime data manifest entry.' }
  $expected = $Matches[1]; $file = $Matches[2]
  $path = Join-Path $source "rime-data/$file"
  if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected) { throw "Rime data checksum mismatch: $file" }
  Copy-PackageFile $path "rime-data/$file"
}
Copy-PackageFile (Join-Path $source 'rime-data/SHA256SUMS') 'rime-data/SHA256SUMS'
foreach ($file in @('jieba.dict.utf8','hmm_model.utf8','user.dict.utf8','idf.utf8','stop_words.utf8','pinyin.txt','PINYIN-LICENSE.txt','README.md')) {
  Copy-PackageFile (Join-Path $source "third_party/cppjieba/dict/$file") "third_party/cppjieba/dict/$file"
}
Copy-PackageFile (Join-Path $source 'third_party/cppjieba/LICENSE') 'third_party/cppjieba/LICENSE'
foreach ($file in @('README.md','LICENSE','NOTICE','THIRD_PARTY_NOTICES.md',"docs/releases/v$version.md", "docs/releases/v$version-validation.md",'docs/design/brand/windows-taskbar.png')) {
  Copy-PackageFile (Join-Path $source $file) $file
}
Copy-PackageFile (Join-Path $source 'docs/research/runtime-model-regression-2026-11-16.json') 'docs/research/runtime-model-regression-2026-11-16.json'
Copy-PackageFile (Join-Path $source 'docs/research/code-review-2026-11-16.md') 'docs/research/code-review-2026-11-16.md'
Copy-PackageFile (Join-Path $source 'docs/plans/voice-input-optimization.md') 'docs/plans/voice-input-optimization.md'
Copy-PackageFile (Join-Path $source 'docs/research/voice-runtime-2026-11-21.md') 'docs/research/voice-runtime-2026-11-21.md'
foreach ($file in @('install.cmd','uninstall.cmd','check.cmd','START-HERE.txt','qwen-cpu.conf')) {
  Copy-PackageFile (Join-Path $source "packaging/$file") $file
}
foreach ($file in @('setup.ps1','download_models.ps1','dependencies.json','verify_package.ps1','upgrade_status.ps1')) {
  Copy-PackageFile (Join-Path $source "tools/$file") "tools/$file"
}
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $source 'licenses') -Filter '*.txt' -File) {
  Copy-PackageFile $file.FullName ("licenses/" + $file.Name)
}
foreach ($skin in @('titanium','daylight','deep-space')) {
  foreach ($kind in @('voice','candidate')) {
    $file = "docs/design/skins/$skin-$kind.png"
    Copy-PackageFile (Join-Path $source $file) $file
  }
}

$files = @(Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName | ForEach-Object {
  [ordered]@{path=$_.FullName.Substring($package.Length + 1).Replace('\','/');size=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$manifest = [ordered]@{product='Simple Voice';version=$version;platform='windows-x64';channel='release-candidate';modelsBundled=$false;files=$files}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'package-manifest.json') -Encoding UTF8
& (Join-Path $source 'tools/verify_package.ps1') -Root $package -RequireSigned:$RequireSigned
$zip = Join-Path $output ($name + '.zip')
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal -Force
$digest = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($zip + '.sha256'), "$digest  $name.zip`n", [Text.UTF8Encoding]::new($false))
Write-Output "Package: $zip"
Write-Output "SHA-256: $digest"
Write-Output "Files: $($files.Count + 1)"
# Staging is disposable once the verified archive and checksum exist.
$stagePath = [IO.Path]::GetFullPath($stage)
if (-not $stagePath.StartsWith($output.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($stagePath) -notmatch '^\.stage-[0-9a-f]{32}$') { throw 'Unsafe staging cleanup path.' }
Remove-Item -LiteralPath $stagePath -Recurse -Force
