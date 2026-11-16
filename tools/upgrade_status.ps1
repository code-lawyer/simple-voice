# Read-only inventory. An inaccessible process is reported, never assumed clean.
function Get-SimpleVoiceUpgradeState([string]$PackageRoot) {
  $expected = [IO.Path]::GetFullPath($PackageRoot).TrimEnd('\') + '\'
  $session = (Get-Process -Id $PID).SessionId
  $rows = @()
  $unreadable = 0
  foreach ($process in Get-Process) {
    if ($process.SessionId -ne $session -or $process.Id -eq $PID) { continue }
    try {
      if ($process.ProcessName -eq 'voiceime_engine') {
        $path = $process.MainModule.FileName
        if (-not $path.StartsWith($expected, [StringComparison]::OrdinalIgnoreCase)) {
          $rows += [pscustomobject]@{Pid=$process.Id;Process=$process.ProcessName;Kind='Engine';Path=$path}
        }
      }
      foreach ($module in $process.Modules) {
        if ($module.ModuleName -eq 'voiceime_tsf.dll' -and
            -not $module.FileName.StartsWith($expected, [StringComparison]::OrdinalIgnoreCase)) {
          $rows += [pscustomobject]@{Pid=$process.Id;Process=$process.ProcessName;Kind='Loaded DLL';Path=$module.FileName}
        }
      }
    } catch { $unreadable++ }
  }
  [pscustomobject]@{OldComponents=@($rows);UnreadableProcesses=$unreadable;RequiresSignOut=($rows.Count -gt 0)}
}
