#pragma once

inline int RegisterExitCode(bool hkcu_written, bool hklm_written) {
  return hkcu_written && hklm_written ? 0 : 1;
}

inline int UnregisterExitCode(bool removed) { return removed ? 0 : 1; }

inline bool ShouldLaunchEngine(bool engine_mutex_exists) { return !engine_mutex_exists; }
