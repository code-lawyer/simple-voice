#pragma once

#include "composer.hpp"

#include <cstdint>
#include <string>

class RimeSession : public Composer {
 public:
  RimeSession() = default;
  ~RimeSession() override;

  RimeSession(const RimeSession&) = delete;
  RimeSession& operator=(const RimeSession&) = delete;

  bool Start(const std::wstring& shared_dir, const std::wstring& user_dir);
  void Shutdown();

  RimeView ProcessKey(int vk, bool shift) override;
  void Clear() override;
  const RimeView& view() const override;

 private:
  void Refresh();

  void* api_ = nullptr;
  std::uintptr_t session_ = 0;
  bool ready_ = false;
  std::string shared_utf8_;
  std::string user_utf8_;
  RimeView view_{};
};
