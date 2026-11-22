#include "pinyin_client.hpp"

namespace {
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
  if (!data.empty()) return false;
  view = std::move(next);
  return true;
}
}

std::string EncodePinyinView(const RimeView& view) {
  std::string out;
  out.push_back((view.composing ? 1 : 0) | (view.ascii_punctuation ? 2 : 0));
  out.push_back(static_cast<char>(view.highlight));
  out.push_back(static_cast<char>(view.page.size()));
  Append16(out, view.caret);
  auto put = [&](const std::string& field) { Append16(out, field.size()); out += field; };
  put(view.preedit);
  put(view.commit);
  for (const auto& term : view.page) put(term);
  return out;
}

bool PinyinClient::Start(const std::wstring& name, const std::wstring& engine, HWND window) {
  if (connected()) return true;
  if (name.empty() || !window) return false;
  const auto until = GetTickCount64() + 650;
  do {
    pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (connected()) break;
    const auto error = GetLastError();
    if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return false;
    if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 25);
    else Sleep(15);
  } while (GetTickCount64() < until);
  if (!connected()) return false;
  auto peer = PipeServerPeer(pipe_);
  if (!peer || !peer->is_image(engine)) { Shutdown(); return false; }
  Message hello{MsgType::PinyinHello};
  hello.client_window = reinterpret_cast<std::uintptr_t>(window);
  return Exchange(hello, 5000);
}

void PinyinClient::Shutdown() {
  if (connected()) CloseHandle(pipe_);
  pipe_ = INVALID_HANDLE_VALUE;
  view_ = {};
}

bool PinyinClient::Exchange(const Message& request, DWORD timeout) {
  PipeDeadline deadline(timeout);
  if (connected() && deadline.event() && WritePipeBytes(pipe_, Encode(request), deadline.event())) {
    std::string bytes;
    for (;;) {
      auto part = ReadPipeChunk(pipe_, nullptr, deadline.event());
      bytes += part.bytes;
      Message reply;
      std::size_t used = 0;
      const auto status = DecodeFrame(bytes, &reply, &used);
      if (status == DecodeStatus::Complete && used == bytes.size() &&
          reply.type == MsgType::PinyinView && ReadView(reply.text, view_)) return true;
      if (status != DecodeStatus::Incomplete || part.status != PipeReadStatus::Data) break;
    }
  }
  Shutdown();
  return false;
}
RimeView PinyinClient::ProcessKey(int vk, bool shift) {
  return ProcessKeyWithModifiers(vk, shift, false, false);
}
RimeView PinyinClient::ProcessKeyWithModifiers(int vk, bool shift, bool ctrl, bool alt) {
  Message request{MsgType::PinyinKey};
  request.text.push_back(static_cast<char>(vk));
  request.text.push_back((shift ? 1 : 0) | (ctrl ? 2 : 0) | (alt ? 4 : 0));
  Exchange(request, 1000);
  return view_;
}
void PinyinClient::Clear() {
  if (connected() && view_.composing) Exchange(Message{MsgType::PinyinClear}, 1000);
  view_.commit.clear();
}
void PinyinClient::ConfirmInserted(const std::string& text) {
  if (!connected() || text.empty() || view_.commit != text) return;
  PipeDeadline deadline(500);
  if (!deadline.event() || !WritePipeBytes(pipe_, Encode({MsgType::PinyinInserted}), deadline.event())) Shutdown();
  else view_.commit.clear();
}
