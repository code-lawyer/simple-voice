#pragma once
#include "pipe_io.hpp"
#include "ipc.hpp"
#include <memory>

// Logon-scoped desktop IPC. This cannot isolate code injected into a trusted host.
std::wstring VoicePipeName();
std::wstring VoiceMutexName();
std::wstring ExecutableDirectory();
bool SameFile(const std::wstring& first, const std::wstring& second);

class PipeSecurity {
 public:
  PipeSecurity();
  ~PipeSecurity();
  PipeSecurity(const PipeSecurity&) = delete;
  PipeSecurity& operator=(const PipeSecurity&) = delete;
  SECURITY_ATTRIBUTES* attributes();  // nullptr means initialization failed.
 private:
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  SECURITY_ATTRIBUTES attributes_{};
};

class IpcPeer {
 public:
  IpcPeer(HANDLE process, DWORD id) : process_(process), id_(id) {}
  ~IpcPeer();
  IpcPeer(const IpcPeer&) = delete;
  IpcPeer& operator=(const IpcPeer&) = delete;
  DWORD id() const { return id_; }
  bool alive() const;
  bool owns_window(HWND window, const std::wstring& expected_module) const;
  bool is_image(const std::wstring& expected_image) const;
 private:
  HANDLE process_ = nullptr;
  DWORD id_ = 0;
};
std::shared_ptr<IpcPeer> PipeClientPeer(HANDLE pipe);
std::shared_ptr<IpcPeer> PipeServerPeer(HANDLE pipe);
DWORD ForegroundProcessId();

class PipeDeadline {
 public:
  explicit PipeDeadline(DWORD milliseconds = 3000);
  ~PipeDeadline();
  PipeDeadline(const PipeDeadline&) = delete;
  PipeDeadline& operator=(const PipeDeadline&) = delete;
  HANDLE event() const { return timer_; }
 private:
  HANDLE timer_ = nullptr;
};
bool ClientHandshake(HANDLE pipe, HWND window, HANDLE stop, Message& initial, std::string& remaining);
