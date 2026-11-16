#pragma once

inline int RegisterExitCode(bool user_enabled, bool machine_registered) {
  return user_enabled && machine_registered ? 0 : 1;
}

inline int UnregisterExitCode(bool removed) { return removed ? 0 : 1; }

inline bool ShouldLaunchEngine(bool engine_mutex_exists) { return !engine_mutex_exists; }
