param(
    [string]$WorkRoot = (Join-Path $PSScriptRoot '../../build/asr-benchmark'),
    [string]$ResultsRoot,
    [string]$Compiler = 'g++',
    [ValidateRange(1, 64)][int]$Threads = 2,
    [ValidateRange(1, 100)][int]$Repeats = 3,
    [string[]]$Configurations,
    [ValidateSet('main', 'stress', 'all')][string]$Suite = 'main',
    [switch]$Include2048
)
$ErrorActionPreference = 'Stop'
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$layout = Get-Content -LiteralPath (Join-Path $WorkRoot 'layout.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'models.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$corpus = Get-Content -LiteralPath (Join-Path $WorkRoot 'corpus.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $Configurations) { $Configurations = @($layout.requestedConfigurations) }
if (-not $ResultsRoot) { $ResultsRoot = Join-Path $WorkRoot ('results/' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')) }
$ResultsRoot = [IO.Path]::GetFullPath($ResultsRoot)
if ((Test-Path -LiteralPath $ResultsRoot) -and @(Get-ChildItem -LiteralPath $ResultsRoot -Force).Count -gt 0) {
    throw 'ResultsRoot is not empty. Choose a new directory; previous measurements are never overwritten.'
}
$runtime = [string]$layout.artifacts.runtime
if (-not $runtime) { throw 'Run prepare.mjs before the benchmark.' }
& node (Join-Path $PSScriptRoot 'verify-layout.mjs') '--work-root' $WorkRoot
if ($LASTEXITCODE -ne 0) { throw 'Prepared assets changed; run preparation again before measuring.' }
$binaryRoot = Join-Path $WorkRoot 'bin'
New-Item -ItemType Directory -Path $binaryRoot -Force | Out-Null
New-Item -ItemType Directory -Path $ResultsRoot -Force | Out-Null
$binary = Join-Path $binaryRoot 'asr-benchmark.exe'
$source = Join-Path $PSScriptRoot 'benchmark.cpp'
# Static MinGW C++/pthread runtime; only the explicitly loaded sherpa DLL is external.
$previousPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & $Compiler '-O2' '-std=c++17' '-Wall' '-Wextra' '-static' '-municode' '-I' (Join-Path $runtime 'include') $source '-o' $binary '-lpsapi'
    $compilerExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousPreference
}
if ($compilerExitCode -ne 0) { throw 'Benchmark compilation failed' }

$plan = @()
if ($Suite -eq 'main' -or $Suite -eq 'all') {
    foreach ($kind in $Configurations) {
        $config = $manifest.configurations | Where-Object { $_.kind -eq $kind } | Select-Object -First 1
        if (-not $config) { throw "Unknown configuration: $kind" }
        $model = [string]$layout.artifacts.($config.artifact)
        if (-not $model) { throw "Model not prepared: $($config.artifact)" }
        $plan += [pscustomobject]@{ suite = 'main'; kind = $kind; model = $model; corpus = (Join-Path $WorkRoot 'corpus.tsv'); runs = @(1..$Repeats); ids = @($corpus.cases.id) }
    }
}
if ($Suite -eq 'stress' -or $Suite -eq 'all') {
    if (-not $layout.artifacts.qwen) { throw 'Prepare qwen before the stress suite.' }
    & node (Join-Path $PSScriptRoot 'stress.mjs') '--work-root' $WorkRoot
    if ($LASTEXITCODE -ne 0) { throw 'Stress corpus generation failed' }
    $stress = Get-Content -LiteralPath (Join-Path $WorkRoot 'stress.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($name in @('stress_whole', 'stress20', 'stress15')) {
        $plan += [pscustomobject]@{ suite = $name; kind = 'qwen'; model = [string]$layout.artifacts.qwen; corpus = (Join-Path $WorkRoot ($name + '.tsv')); runs = @(1..$Repeats); ids = @($stress.suites.$name.id) }
    }
    if ($Include2048) {
        $plan += [pscustomobject]@{ suite = 'stress_whole'; kind = 'qwen_debug_2048'; model = [string]$layout.artifacts.qwen; corpus = (Join-Path $WorkRoot 'stress_whole.tsv'); runs = @(1); ids = @($stress.suites.stress_whole.id) }
    }
}
$compilerVersion = (& $Compiler '--version' | Select-Object -First 1)
$stressManifestSha256 = $null
if ($Suite -eq 'stress' -or $Suite -eq 'all') {
    $stressManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $WorkRoot 'stress.json') -Algorithm SHA256).Hash.ToLowerInvariant()
}
[ordered]@{
    schema = 1; utc = [DateTime]::UtcNow.ToString('o'); threads = $Threads; provider = 'cpu'
    compiler = [string]$compilerVersion; sourceSha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    modelManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'models.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    corpusManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $WorkRoot 'corpus.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    stressManifestSha256 = $stressManifestSha256
    plan = $plan
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ResultsRoot 'run.json') -Encoding UTF8

# Serial, round-robin runs: never benchmark models concurrently with each other.
for ($iteration = 1; $iteration -le $Repeats; ++$iteration) {
    foreach ($item in $plan) {
        if ($item.runs -notcontains $iteration) { continue }
        $prefix = Join-Path $ResultsRoot ($item.suite + '-' + $item.kind + '-' + $iteration)
        # Windows PowerShell 5.1 wraps native stderr in ErrorRecord, even for
        # informational resampler messages. The process exit code decides success.
        $previousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & $binary $item.kind $item.model $item.corpus ($prefix + '.jsonl') $iteration (Join-Path $runtime 'lib/sherpa-onnx-c-api.dll') $Threads $item.suite 1> ($prefix + '.stdout.log') 2> ($prefix + '.stderr.log')
            $processExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
        if ($processExitCode -ne 0) { throw "Benchmark failed: $prefix. Partial output was retained." }
        Write-Output ("Completed {0} / {1} / run {2}" -f $item.suite, $item.kind, $iteration)
    }
}
& node (Join-Path $PSScriptRoot 'score.mjs') '--work-root' $WorkRoot '--results-root' $ResultsRoot
if ($LASTEXITCODE -ne 0) { throw 'Scoring or completeness validation failed' }
Write-Output ("Results: {0}" -f $ResultsRoot)
