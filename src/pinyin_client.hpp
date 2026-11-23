#pragma once
#include "composer.hpp"
#include "ipc_security.hpp"

// A text service keeps its own composition. All application sessions run in
// the same engine so librime can open the existing learning database once.
class PinyinClient final : public Composer {
 public:
  PinyinClient() = default;
  PinyinClient(const PinyinClient&) = delete;
  PinyinClient& operator=(const PinyinClient&) = delete;
  ~PinyinClient() { Shutdown(); }
  void Configure(std::wstring name, std::wstring engine, HWND window);
  // Starts or observes a background handshake without waiting for Rime deploy.
  bool EnsureConnected();
  void Shutdown();
  bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }
  RimeView ProcessKey(int vk, bool shift) override;
  RimeView ProcessKeyWithModifiers(int vk, bool shift, bool ctrl, bool alt) override;
  void Clear() override;
  void ConfirmInserted(const std::string& text);
  const std::string& interrupted_text() const { return interrupted_text_; }
  void ClearInterruptedText() { interrupted_text_.clear(); }
  const RimeView& view() const override { return view_; }

 private:
  static DWORD WINAPI ConnectThread(void* self);
  void Connect();
  void Disconnect();
  bool Exchange(const Message& request);

  std::wstring name_, engine_;
  HWND window_ = nullptr;
  HANDLE connector_ = nullptr, cancel_connect_ = nullptr;
  ULONGLONG retry_after_ = 0;
  // The worker owns these until its thread handle signals completion.
  HANDLE pending_pipe_ = INVALID_HANDLE_VALUE;
  RimeView pending_view_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  RimeView view_;
  std::string interrupted_text_;
};

std::string EncodePinyinView(const RimeView& view, bool include_raw = false);
