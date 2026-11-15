param(
  [ValidateSet('Install', 'Uninstall', 'Check')][string]$Action = 'Install',
  [switch]$Offline
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$clsid = '{A4C1E2B0-7D34-4F91-9A62-5E18C3704B21}'

try {
  if (-not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) {
    throw 'Simple Voice requires 64-bit Windows and 64-bit PowerShell.'
  }
  if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { throw 'This preview supports Intel/AMD x64 Windows only.' }
  $engine = Join-Path $root 'voiceime_engine.exe'
  $registrar = Join-Path $root 'voiceime_register.exe'
  $dll = Join-Path $root 'voiceime_tsf.dll'
  foreach ($file in @($engine, $registrar, $dll)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Extract the complete release ZIP first. Missing: $file" }
  }

  if ($Action -ne 'Uninstall') {
    & (Join-Path $PSScriptRoot 'download_models.ps1') -Root $root -ForPackage -Offline:$Offline
    # The official sherpa runtime uses Microsoft's VC++ runtime; do not copy
    # arbitrary system DLLs into the package or silently install extra software.
    foreach ($name in @('msvcp140.dll','msvcp140_1.dll','vcruntime140.dll','vcruntime140_1.dll')) {
      if (-not (Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) $name))) {
        throw 'Install Microsoft Visual C++ x64 Redistributable, then run install.cmd again: https://aka.ms/vc14/vc_redist.x64.exe'
      }
    }
    & $engine --check
    if ($LASTEXITCODE -ne 0) { throw 'Local model initialization failed. Check the message above and the Visual C++ x64 runtime.' }
    if ($Action -eq 'Check') {
      Write-Host 'Check complete. No microphone or system registration was used.'
      exit 0
    }
  }

  $key = "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$clsid\InProcServer32"
  if ($Action -eq 'Uninstall') {
    $registeredAny = $false
    foreach ($hive in @('HKEY_CURRENT_USER', 'HKEY_LOCAL_MACHINE')) {
      $registeredKey = "Registry::$hive\Software\Classes\CLSID\$clsid\InProcServer32"
      if (Test-Path -LiteralPath $registeredKey) {
        $registeredAny = $true
        $registered = (Get-Item -LiteralPath $registeredKey).GetValue('')
        if (-not [String]::Equals($registered, $dll, [StringComparison]::OrdinalIgnoreCase)) {
          throw 'A different Simple Voice folder is currently installed. Run uninstall.cmd from that folder to avoid removing a newer version.'
        }
      }
    }
    if (-not $registeredAny) { Write-Host 'Simple Voice is already unregistered. Personal data was kept.'; exit 0 }
  }
  Write-Host 'Windows will request administrator permission for input-method registration.'
  Write-Host 'Approve using the same Windows account. The package stays in this folder.'
  if ($Action -eq 'Uninstall') {
    $process = Start-Process -FilePath $registrar -ArgumentList '--unregister' -Verb RunAs -WindowStyle Hidden -PassThru -Wait
  } else {
    $process = Start-Process -FilePath $registrar -Verb RunAs -WindowStyle Hidden -PassThru -Wait
  }
  if ($process.ExitCode -ne 0) { throw "Registration failed (exit $($process.ExitCode)). The Windows administrator prompt must be accepted." }
  if ($Action -eq 'Install') {
    if (-not (Test-Path -LiteralPath $key) -or
        -not [String]::Equals((Get-Item -LiteralPath $key).GetValue(''), $dll, [StringComparison]::OrdinalIgnoreCase)) {
      throw 'Registration did not update this account. Administrator elevation must use the same Windows account.'
    }
    Write-Host 'Installed Simple Voice. Sign out of Windows and sign in again, then choose Simple Voice / Qingyin with Win+Space.'
    Write-Host 'Keep this folder in place. Sign-in refreshes any previously loaded input-method DLLs.'
  } else {
    if (Test-Path -LiteralPath $key) { throw 'Registration still exists for this account. Uninstall was not completed.' }
    Write-Host 'Unregistered Simple Voice. Sign out and in before deleting the extracted application folder.'
    Write-Host 'Personal settings and dictionaries remain in %LOCALAPPDATA%\VoiceIme.'
  }
} catch {
  Write-Host ("ERROR: " + $_.Exception.Message) -ForegroundColor Red
  exit 1
}
