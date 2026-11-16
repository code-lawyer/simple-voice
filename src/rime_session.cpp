#include "rime_session.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "rime_api.h"
#include "settings.hpp"
#include "product.hpp"

#include <filesystem>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
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
  std::string personal_generation;
  std::uint64_t personal_stamp=0;
};

RimeRuntime& Runtime() {
  static RimeRuntime runtime;
  return runtime;
}

void ConfigurePersonal(RimeApi* api,RimeRuntime& runtime) {
  const auto path=std::filesystem::path(runtime.user_path)/"voiceime_personal.txt";
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes)) return;
  const auto stamp=(std::uint64_t(attributes.ftLastWriteTime.dwHighDateTime)<<32)|attributes.ftLastWriteTime.dwLowDateTime;
  if(stamp==runtime.personal_stamp && !runtime.personal_generation.empty()) return;
  if(attributes.nFileSizeHigh || attributes.nFileSizeLow>8*1024*1024) return;
  std::ifstream input(path,std::ios::binary);
  if(!input) return;
  std::string content((std::istreambuf_iterator<char>(input)),{});
  std::uint64_t hash=14695981039346656037ULL;
  for(unsigned char c:content) { hash^=c; hash*=1099511628211ULL; }
  std::ostringstream id; id<<"voiceime_personal_g"<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;
  const auto generation=id.str();
  const auto target=std::filesystem::path(runtime.user_path)/(generation+".txt");
  if(!std::filesystem::exists(target)) {
    // Hash-addressed immutable snapshots let one live session finish its
    // composition while the next session opens the newly edited dictionary.
    const auto temporary=target.wstring()+L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    { std::ofstream output(std::filesystem::path(temporary),std::ios::binary); if(!output) return;
      output.write(content.data(),content.size()); output.flush(); if(!output) return; }
    if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temporary.c_str()); return; }
  }
  runtime.personal_generation=generation; runtime.personal_stamp=stamp;
  // These files are generated snapshots, never the authoritative user terms.
  // Loaded stabledb objects keep their data in memory after the file closes.
  std::error_code error;
  for(const auto& entry:std::filesystem::directory_iterator(runtime.user_path,error)) {
    const auto name=entry.path().filename().string();
    if(name.size()==39 && name.rfind("voiceime_personal_g",0)==0 && name.substr(name.size()-4)==".txt" && entry.path()!=target) {
      std::filesystem::remove(entry.path(),error); error.clear();
    }
  }
}

uintptr_t CreateConfiguredSession(RimeApi* api,bool learning,const std::string& generation) {
  RimeConfig main{},private_config{};
  const bool have_main=api->schema_open("voiceime",&main)!=0;
  const bool have_private=api->schema_open("voiceime_private",&private_config)!=0;
  // Schema configs are weak-cached by librime. Keep both handles alive until
  // the new engine owns the selected schema, including the private schema.
  if(!generation.empty()) {
    if(have_main) api->config_set_string(&main,"personal/user_dict",generation.c_str());
    if(have_private) api->config_set_string(&private_config,"personal/user_dict",generation.c_str());
  }
  auto session=api->create_session();
  if(session && !api->select_schema(session,learning?"voiceime":"voiceime_private")) { api->destroy_session(session); session=0; }
  if(have_main) api->config_close(&main); if(have_private) api->config_close(&private_config);
  return session;
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
      *keycode = 0xff0d;
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
    case 0x23: *keycode=0xff57; break; // End
    case 0x24: *keycode=0xff50; break; // Home
    case 0x25: *keycode=0xff51; break;
    case 0x26: *keycode=0xff52; break;
    case 0x27: *keycode=0xff53; break;
    case 0x28: *keycode=0xff54; break;
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
    traits.distribution_version = SIMPLE_VOICE_VERSION;
    traits.app_name = "rime.voiceime";
    traits.min_log_level = 2;

    api->setup(&traits);
    api->initialize(&traits);
    api->deployer_initialize(&traits);
    if (api->start_maintenance(1) && api->join_maintenance_thread != nullptr) {
      api->join_maintenance_thread();
    }
  }

  ConfigurePersonal(api,runtime);
  learning_=LoadInputPreferences((std::filesystem::path(user_path).parent_path()/"preferences.ini").wstring()).learning;
  personal_generation_=runtime.personal_generation;
  const uintptr_t session=CreateConfiguredSession(api,learning_,runtime.personal_generation);
  if (session == 0) {
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
    runtime.personal_generation.clear(); runtime.personal_stamp=0;
  }
  api_ = nullptr;
  ready_ = false;
  view_ = {};
}

RimeView RimeSession::ProcessKey(int vk, bool shift) {
  return ProcessKeyWithModifiers(vk,shift,false,false);
}

RimeView RimeSession::ProcessKeyWithModifiers(int vk, bool shift, bool ctrl, bool alt) {
  std::lock_guard<std::mutex> lock(Runtime().mutex);
  if(ready_ && !view_.composing) RefreshResources();
  if (ready_ && api_ != nullptr && session_ != 0) {
    auto* api = static_cast<RimeApi*>(api_);
    if (vk == VK_RETURN && !ctrl && !alt && view_.composing) {
      // Rime's express_editor commits only the prefix before its caret.
      // Enter in Simple Voice preserves the entire edited raw input instead.
      const char* input = api->get_input(session_);
      const std::string raw = input ? input : "";
      api->clear_composition(session_);
      Refresh();
      view_.commit = raw;
      return view_;
    }
    int keycode = 0;
    int mask = 0;
    MapKey(vk, shift, &keycode, &mask);
    if(ctrl) mask |= 4;
    if(alt) mask |= 8;
    if (keycode != 0) {
      static_cast<RimeApi*>(api_)->process_key(session_, keycode, mask);
    }
  }
  Refresh();
  return view_;
}

bool RimeSession::SetLearning(bool enabled) {
  std::lock_guard<std::mutex> lock(Runtime().mutex);
  if(!ready_ || !api_) return false;
  auto* api=static_cast<RimeApi*>(api_);
  const bool ok=api->select_schema(session_,enabled?"voiceime":"voiceime_private")!=0;
  if(ok) { learning_=enabled; Refresh(); }
  return ok;
}

void RimeSession::RefreshResources() {
  auto& runtime=Runtime(); auto* api=static_cast<RimeApi*>(api_);
  ConfigurePersonal(api,runtime);
  const auto next=LoadInputPreferences((std::filesystem::path(runtime.user_path).parent_path()/"preferences.ini").wstring()).learning;
  if(learning_==next && personal_generation_==runtime.personal_generation) return;
  const auto replacement=CreateConfiguredSession(api,next,runtime.personal_generation);
  if(!replacement) {
    if(replacement) api->destroy_session(replacement); return;
  }
  api->destroy_session(session_); session_=replacement;
  personal_generation_=runtime.personal_generation; learning_=next; Refresh();
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
    view_.caret=context.composition.cursor_pos;
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
