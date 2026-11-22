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
  bool Start(const std::wstring& name, const std::wstring& engine, HWND window);
  void Shutdown();
  bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }
  RimeView ProcessKey(int vk, bool shift) override;
  RimeView ProcessKeyWithModifiers(int vk, bool shift, bool ctrl, bool alt) override;
  void Clear() override;
  void ConfirmInserted(const std::string& text);
  const RimeView& view() const override { return view_; }

 private:
  bool Exchange(const Message& request, DWORD timeout);
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  RimeView view_;
};

std::string EncodePinyinView(const RimeView& view);
