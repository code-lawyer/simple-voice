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
  RimeView ProcessKeyWithModifiers(int vk, bool shift, bool ctrl, bool alt) override;
  bool SetLearning(bool enabled);
  void Clear() override;
  const RimeView& view() const override;

 private:
  void Refresh();
  void RefreshResources(); // Caller holds the process-wide librime lock.

  void* api_ = nullptr;
  std::uintptr_t session_ = 0;
  bool ready_ = false;
  RimeView view_{};
  bool learning_=true;
  std::string personal_generation_;
};
