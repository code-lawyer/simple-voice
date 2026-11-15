#include "rime_session.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "rime_api.h"

#include <filesystem>
#include <mutex>
#include <windows.h>

namespace {

// librime owns one runtime per process, not one runtime per text service.
// Serialize its API across UI threads and finalize only after the last session.
struct RimeRuntime {
  std::mutex mutex;
  std::size_t sessions = 0;
  std::wstring shared_path;
  std::wstring user_path;
  std::string shared_utf8;
  std::string user_utf8;
};

RimeRuntime& Runtime() {
  static RimeRuntime runtime;
  return runtime;
}

constexpr int kShiftMask = 1;
constexpr int kKeyBackSpace = 0xff08;
constexpr int kKeyEscape = 0xff1b;
constexpr int kKeyDelete = 0xffff;
constexpr int kKeyPrior = 0xff55;
constexpr int kKeyNext = 0xff56;

std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) return {};
  std::string utf8(static_cast<size_t>(bytes - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
  return utf8;
}

int Shifted(bool shift, int plain, int shifted) {
  return shift ? shifted : plain;
}

// Windows virtual keys are not librime keysyms. Letters must be lowercase:
// the schema alphabet is lowercase, and express_editor binds Return to the
// raw spelling. Space confirms the highlighted candidate.
void MapKey(int vk, bool shift, int* keycode, int* mask) {
  *keycode = 0;
  *mask = shift ? kShiftMask : 0;
  if (vk >= 0x41 && vk <= 0x5A) {
    *keycode = shift ? vk : vk + 0x20;
    return;
  }
  if (vk >= 0x30 && vk <= 0x39) {
    if (shift) {
      // half_shape is keyed by the character, so the shift mask is cleared.
      static const int kShiftedDigit[] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
      *keycode = kShiftedDigit[vk - 0x30];
      *mask = 0;
    } else {
      *keycode = vk;
    }
    return;
  }
  // 0x61 is 'a'. Numpad 0-9 are XK_KP_0 (0xffb0) through XK_KP_9.
  if (vk >= 0x60 && vk <= 0x69) {
    *keycode = 0xffb0 + (vk - 0x60);
    return;
  }
  switch (vk) {
    case 0x08:
      *keycode = kKeyBackSpace;
      *mask = 0;
      break;
    case 0x0D:
      *keycode = 0x20;
      *mask = 0;
      break;
    case 0x1B:
      *keycode = kKeyEscape;
      *mask = 0;
      break;
    case 0x20:
      *keycode = 0x20;
      *mask = 0;
      break;
    case 0x21:
      *keycode = kKeyPrior;
      *mask = 0;
      break;
    case 0x22:
      *keycode = kKeyNext;
      *mask = 0;
      break;
    case 0x2E:
      *keycode = kKeyDelete;
      *mask = 0;
      break;
    case 0xBA:
      *keycode = Shifted(shift, ';', ':');
      break;
    case 0xBB:
      *keycode = Shifted(shift, '=', '+');
      break;
    case 0xBC:
      *keycode = Shifted(shift, ',', '<');
      break;
    case 0xBD:
      *keycode = Shifted(shift, '-', '_');
      break;
    case 0xBE:
      *keycode = Shifted(shift, '.', '>');
      break;
    case 0xBF:
      *keycode = Shifted(shift, '/', '?');
      break;
    case 0xC0:
      *keycode = Shifted(shift, '`', '~');
      break;
    case 0xDB:
      *keycode = Shifted(shift, '[', '{');
      break;
    case 0xDC:
      *keycode = Shifted(shift, '\\', '|');
      break;
    case 0xDD:
      *keycode = Shifted(shift, ']', '}');
      break;
    case 0xDE:
      *keycode = Shifted(shift, '\'', '"');
      break;
    default:
      if (vk >= 0x20 && vk < 0x7F) *keycode = vk;
      break;
  }
}

}  // namespace

RimeSession::~RimeSession() {
  Shutdown();
}

bool RimeSession::Start(const std::wstring& shared_dir, const std::wstring& user_dir) {
  Shutdown();
  RimeRuntime& runtime = Runtime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  RimeApi* api = rime_get_api();
  if (api == nullptr || api->setup == nullptr || api->initialize == nullptr ||
      api->deployer_initialize == nullptr || api->start_maintenance == nullptr ||
      api->create_session == nullptr || api->select_schema == nullptr) {
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(user_dir), ec);
  if (ec) return false;
  const auto shared_path = std::filesystem::weakly_canonical(shared_dir, ec).wstring();
  if (ec) return false;
  const auto user_path = std::filesystem::weakly_canonical(user_dir, ec).wstring();
  if (ec) return false;
  if (runtime.sessions != 0 &&
      (_wcsicmp(runtime.shared_path.c_str(), shared_path.c_str()) != 0 ||
       _wcsicmp(runtime.user_path.c_str(), user_path.c_str()) != 0)) {
    // Reconfiguring a live runtime would invalidate another text service.
    return false;
  }

  if (runtime.sessions == 0) {
    runtime.shared_path = shared_path;
    runtime.user_path = user_path;
    runtime.shared_utf8 = WideToUtf8(shared_path);
    runtime.user_utf8 = WideToUtf8(user_path);
    if (runtime.shared_utf8.empty() || runtime.user_utf8.empty()) return false;
    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = runtime.shared_utf8.c_str();
    traits.user_data_dir = runtime.user_utf8.c_str();
    traits.distribution_name = "Simple Voice";
    traits.distribution_code_name = "voiceime";
    traits.distribution_version = "0.1.0";
    traits.app_name = "rime.voiceime";
    traits.min_log_level = 2;

    api->setup(&traits);
    api->initialize(&traits);
    api->deployer_initialize(&traits);
    if (api->start_maintenance(1) && api->join_maintenance_thread != nullptr) {
      api->join_maintenance_thread();
    }
  }

  const uintptr_t session = api->create_session();
  if (session == 0 || !api->select_schema(session, "voiceime")) {
    if (session != 0 && api->destroy_session != nullptr) api->destroy_session(session);
    if (runtime.sessions == 0 && api->finalize != nullptr) api->finalize();
    return false;
  }
  api_ = api;
  session_ = session;
  ready_ = true;
  ++runtime.sessions;
  Refresh();
  return true;
}

void RimeSession::Shutdown() {
  RimeRuntime& runtime = Runtime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (api_ == nullptr && !ready_) return;
  RimeApi* api = static_cast<RimeApi*>(api_);
  if (api != nullptr && session_ != 0 && api->destroy_session != nullptr) {
    api->destroy_session(session_);
  }
  session_ = 0;
  if (ready_ && runtime.sessions > 0 && --runtime.sessions == 0) {
    if (api != nullptr && api->finalize != nullptr) api->finalize();
    runtime.shared_path.clear();
    runtime.user_path.clear();
    runtime.shared_utf8.clear();
    runtime.user_utf8.clear();
  }
  api_ = nullptr;
  ready_ = false;
  view_ = {};
}

RimeView RimeSession::ProcessKey(int vk, bool shift) {
  std::lock_guard<std::mutex> lock(Runtime().mutex);
  if (ready_ && api_ != nullptr && session_ != 0) {
    int keycode = 0;
    int mask = 0;
    MapKey(vk, shift, &keycode, &mask);
    if (keycode != 0) {
      static_cast<RimeApi*>(api_)->process_key(session_, keycode, mask);
    }
  }
  Refresh();
  return view_;
}

void RimeSession::Clear() {
  std::lock_guard<std::mutex> lock(Runtime().mutex);
  if (ready_ && api_ != nullptr && session_ != 0) {
    RimeApi* api = static_cast<RimeApi*>(api_);
    if (api->clear_composition != nullptr) api->clear_composition(session_);
  }
  Refresh();
  view_.commit.clear();
}

const RimeView& RimeSession::view() const {
  return view_;
}

void RimeSession::Refresh() {
  view_ = {};
  if (!ready_ || api_ == nullptr || session_ == 0) return;
  RimeApi* api = static_cast<RimeApi*>(api_);

  RIME_STRUCT(RimeCommit, commit);
  if (api->get_commit != nullptr && api->get_commit(session_, &commit)) {
    if (commit.text != nullptr) view_.commit = commit.text;
    if (api->free_commit != nullptr) api->free_commit(&commit);
  }

  RIME_STRUCT(RimeContext, context);
  if (api->get_context != nullptr && api->get_context(session_, &context)) {
    if (context.composition.preedit != nullptr) view_.preedit = context.composition.preedit;
    int count = context.menu.num_candidates;
    if (count > 5) count = 5;
    if (count < 0) count = 0;
    if (context.menu.candidates != nullptr) {
      view_.page.reserve(static_cast<size_t>(count));
      for (int i = 0; i < count; ++i) {
        const char* text = context.menu.candidates[i].text;
        view_.page.emplace_back(text != nullptr ? text : "");
      }
    }
    view_.highlight = context.menu.highlighted_candidate_index;
    if (view_.highlight < 0 || view_.highlight >= static_cast<int>(view_.page.size())) {
      view_.highlight = 0;
    }
    if (api->free_context != nullptr) api->free_context(&context);
  }

  RIME_STRUCT(RimeStatus, status);
  if (api->get_status != nullptr && api->get_status(session_, &status)) {
    view_.composing = status.is_composing != 0;
    if (api->free_status != nullptr) api->free_status(&status);
  } else {
    view_.composing = !view_.preedit.empty() || !view_.page.empty();
  }
}
