param(
  [string]$BuildDir = 'build-release',
  [string]$OutputDir = 'dist'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$build = [IO.Path]::GetFullPath((Join-Path $source $BuildDir))
$output = [IO.Path]::GetFullPath((Join-Path $source $OutputDir))
$version = '0.1.0'
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

foreach ($file in @('voiceime_engine.exe','voiceime_register.exe','voiceime_tsf.dll')) {
  $path = Join-Path $build $file
  $info = (Get-Item -LiteralPath $path).VersionInfo
  if ($info.ProductName -ne 'Simple Voice' -or $info.FileVersion -ne $version) { throw "Unexpected product/version: $file" }
  Copy-PackageFile $path $file
}
foreach ($file in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-PackageFile (Join-Path $build $file) $file
}
foreach ($file in @('default.yaml','voiceime.schema.yaml','voiceime.dict.yaml','pinyin_simp.dict.yaml','pinyin_simp.AUTHORS','pinyin_simp.LICENSE','SOURCE.txt')) {
  Copy-PackageFile (Join-Path $source "rime-data/$file") "rime-data/$file"
}
foreach ($file in @('jieba.dict.utf8','hmm_model.utf8','user.dict.utf8','idf.utf8','stop_words.utf8','pinyin.txt','PINYIN-LICENSE.txt','README.md')) {
  Copy-PackageFile (Join-Path $source "third_party/cppjieba/dict/$file") "third_party/cppjieba/dict/$file"
}
Copy-PackageFile (Join-Path $source 'third_party/cppjieba/LICENSE') 'third_party/cppjieba/LICENSE'
foreach ($file in @('README.md','LICENSE','NOTICE','THIRD_PARTY_NOTICES.md','PRODUCT.md','DESIGN.md','docs/development.md','docs/releases/v0.1.0.md','docs/releases/v0.1.0-validation.md')) {
  Copy-PackageFile (Join-Path $source $file) $file
}
foreach ($file in @('install.cmd','uninstall.cmd','check.cmd','START-HERE.txt')) {
  Copy-PackageFile (Join-Path $source "packaging/$file") $file
}
foreach ($file in @('setup.ps1','download_models.ps1','dependencies.json')) {
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
$manifest = [ordered]@{product='Simple Voice';version=$version;platform='windows-x64';channel='preview';modelsBundled=$false;files=$files}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'package-manifest.json') -Encoding UTF8
$zip = Join-Path $output ($name + '.zip')
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal -Force
$digest = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($zip + '.sha256'), "$digest  $name.zip`n", [Text.UTF8Encoding]::new($false))
Write-Output "Package: $zip"
Write-Output "SHA-256: $digest"
Write-Output "Files: $($files.Count + 1)"
