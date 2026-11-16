param(
    [Parameter(Mandatory = $true)][string]$WorkRoot,
    [string]$Voice = 'Microsoft Zira Desktop'
)
$ErrorActionPreference = 'Stop'
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$corpus = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'corpus.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Add-Type -AssemblyName System.Speech
$synthesizer = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
    try {
        $synthesizer.SelectVoice($Voice)
    } catch {
        throw "Cannot use Windows voice '$Voice'. Run in a normal Windows terminal with that voice installed, or reuse existing diagnostic WAVs. $($_.Exception.Message)"
    }
    $synthesizer.Rate = 0
    $synthesizer.Volume = 100
    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
    foreach ($case in $corpus.cases) {
        if ($case.acquire.kind -ne 'windows_tts') { continue }
        $target = Join-Path $WorkRoot $case.file
        if (Test-Path -LiteralPath $target) { continue }
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        $synthesizer.SetOutputToWaveFile($target, $format)
        $synthesizer.Speak([string]$case.reference)
        $synthesizer.SetOutputToNull()
    }
    [ordered]@{ voice = $synthesizer.Voice.Name; rate = 0; volume = 100; sampleRate = 16000; purpose = 'Synthetic diagnostic prompts; not human acceptance' } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $WorkRoot 'tts-generation.json') -Encoding UTF8
} finally {
    $synthesizer.Dispose()
}
