#include "pinyin_client.hpp"

#include <utility>

namespace {
constexpr DWORD kKeyTimeoutMs = 60;
constexpr DWORD kRetryDelayMs = 350;

void Append16(std::string& out, std::size_t value) {
  out.push_back(static_cast<char>(value & 255));
  out.push_back(static_cast<char>(value >> 8));
}
unsigned Read16(std::string_view data, std::size_t at) {
  return static_cast<unsigned char>(data[at]) | (static_cast<unsigned char>(data[at+1]) << 8);
}
bool ReadView(std::string_view data, RimeView& view) {
  if (data.size() < 9) return false;
  const unsigned flags = static_cast<unsigned char>(data[0]);
  const unsigned count = static_cast<unsigned char>(data[2]);
  RimeView next;
  next.composing = flags & 1;
  next.ascii_punctuation = flags & 2;
  next.highlight = static_cast<unsigned char>(data[1]);
  next.caret = Read16(data, 3);
  if (flags > 3 || count > 5 || (next.highlight && next.highlight >= count)) return false;
  data.remove_prefix(5);
  auto take = [&](std::string& field) {
    if (data.size() < 2) return false;
    const auto length = Read16(data, 0);
    if (length > data.size() - 2) return false;
    field.assign(data.substr(2, length));
    data.remove_prefix(2 + length);
    return true;
  };
  if (!take(next.preedit) || !take(next.commit)) return false;
  next.page.resize(count);
  for (auto& term : next.page) if (!take(term)) return false;
  if (!data.empty() && !take(next.raw_input)) return false;
  if (!data.empty()) return false;
  view = std::move(next);
  return true;
}

bool ExchangeView(HANDLE pipe, const Message& request, RimeView& view, DWORD timeout,
                  HANDLE stop = nullptr) {
  PipeDeadline deadline(timeout);
  if (!deadline.event() || !WritePipeBytes(pipe, Encode(request), stop, deadline.event())) return false;
  std::string bytes;
  for (;;) {
    auto part = ReadPipeChunk(pipe, deadline.event(), stop);
    bytes += part.bytes;
    Message reply;
    std::size_t used = 0;
    const auto status = DecodeFrame(bytes, &reply, &used);
    if (status == DecodeStatus::Complete)
      return used == bytes.size() && reply.type == MsgType::PinyinView && ReadView(reply.text, view);
    if (status != DecodeStatus::Incomplete || part.status != PipeReadStatus::Data) return false;
  }
}
}

std::string EncodePinyinView(const RimeView& view, bool include_raw) {
  std::string out;
  out.push_back((view.composing ? 1 : 0) | (view.ascii_punctuation ? 2 : 0));
  out.push_back(static_cast<char>(view.highlight));
  out.push_back(static_cast<char>(view.page.size()));
  Append16(out, view.caret);
  auto put = [&](const std::string& field) { Append16(out, field.size()); out += field; };
  put(view.preedit);
  put(view.commit);
  for (const auto& term : view.page) put(term);
  if (include_raw) put(view.raw_input);
  return out;
}

void PinyinClient::Configure(std::wstring name, std::wstring engine, HWND window) {
  Shutdown();
  name_ = std::move(name);
  engine_ = std::move(engine);
  window_ = window;
}

bool PinyinClient::EnsureConnected() {
  if (connected()) return true;
  if (connector_) {
    if (WaitForSingleObject(connector_, 0) != WAIT_OBJECT_0) return false;
    CloseHandle(std::exchange(connector_, nullptr));
    pipe_ = std::exchange(pending_pipe_, INVALID_HANDLE_VALUE);
    if (connected()) {
      view_ = std::move(pending_view_);
      return true;
    }
    retry_after_ = GetTickCount64() + kRetryDelayMs;
  }
  if (!window_ || name_.empty() || GetTickCount64() < retry_after_) return false;
  if (!cancel_connect_) cancel_connect_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (cancel_connect_) connector_ = CreateThread(nullptr, 0, ConnectThread, this, 0, nullptr);
  if (!connector_) retry_after_ = GetTickCount64() + kRetryDelayMs;
  return false;
}

DWORD WINAPI PinyinClient::ConnectThread(void* self) {
  try { static_cast<PinyinClient*>(self)->Connect(); } catch (...) {}
  return 0;
}

void PinyinClient::Connect() {
  auto until = GetTickCount64() + 650;
  auto hello_type = MsgType::PinyinHelloWithRaw;
  while (WaitForSingleObject(cancel_connect_, 0) == WAIT_TIMEOUT) {
    HANDLE handle = CreateFileW(name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      std::unique_ptr<void, decltype(&CloseHandle)> pipe(handle, CloseHandle);
      auto peer = PipeServerPeer(handle);
      if (!peer || !peer->is_image(engine_)) return;
      Message hello{hello_type};
      hello.client_window = reinterpret_cast<std::uintptr_t>(window_);
      if (ExchangeView(handle, hello, pending_view_, 8000, cancel_connect_)) {
        pending_pipe_ = pipe.release();
        return;
      }
      if (hello_type == MsgType::PinyinHello) return;
      // Existing engines close a connection for an unknown hello. Reconnect with their original format.
      hello_type = MsgType::PinyinHello;
      until = GetTickCount64() + 650;
      continue;
    }
    const DWORD error = GetLastError();
    if ((error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) || GetTickCount64() >= until) return;
    if (WaitForSingleObject(cancel_connect_, 20) != WAIT_TIMEOUT) return;
  }
}

void PinyinClient::Disconnect() {
  if (connected()) CloseHandle(std::exchange(pipe_, INVALID_HANDLE_VALUE));
  view_ = {};
  retry_after_ = GetTickCount64() + kRetryDelayMs;
}

void PinyinClient::Shutdown() {
  if (connector_) {
    SetEvent(cancel_connect_);
    WaitForSingleObject(connector_, INFINITE);
    CloseHandle(std::exchange(connector_, nullptr));
  }
  if (cancel_connect_) CloseHandle(std::exchange(cancel_connect_, nullptr));
  if (pending_pipe_ != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(pending_pipe_, INVALID_HANDLE_VALUE));
  pending_view_ = {};
  interrupted_text_.clear();
  Disconnect();
  window_ = nullptr;
  retry_after_ = 0;
}

bool PinyinClient::Exchange(const Message& request) {
  if (connected() && ExchangeView(pipe_, request, view_, kKeyTimeoutMs)) return true;
  if (request.type == MsgType::PinyinKey && view_.composing)
    interrupted_text_ = view_.raw_input.empty() ? view_.preedit : view_.raw_input;
  Disconnect();
  return false;
}
RimeView PinyinClient::ProcessKey(int vk, bool shift) {
  return ProcessKeyWithModifiers(vk, shift, false, false);
}
RimeView PinyinClient::ProcessKeyWithModifiers(int vk, bool shift, bool ctrl, bool alt) {
  Message request{MsgType::PinyinKey};
  request.text.push_back(static_cast<char>(vk));
  request.text.push_back((shift ? 1 : 0) | (ctrl ? 2 : 0) | (alt ? 4 : 0));
  Exchange(request);
  return view_;
}
void PinyinClient::Clear() {
  interrupted_text_.clear();
  if (connected() && view_.composing) Exchange(Message{MsgType::PinyinClear});
  view_.commit.clear();
}
void PinyinClient::ConfirmInserted(const std::string& text) {
  if (!connected() || text.empty() || view_.commit != text) return;
  PipeDeadline deadline(kKeyTimeoutMs);
  if (!deadline.event() || !WritePipeBytes(pipe_, Encode({MsgType::PinyinInserted}), deadline.event())) Disconnect();
  else view_.commit.clear();
}
