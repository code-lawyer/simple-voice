#include "ipc.hpp"

#include <cstring>

namespace {

void WriteLe32(std::string& out, std::size_t at, std::uint32_t value) {
  out[at] = static_cast<char>(value & 0xFF);
  out[at + 1] = static_cast<char>((value >> 8) & 0xFF);
  out[at + 2] = static_cast<char>((value >> 16) & 0xFF);
  out[at + 3] = static_cast<char>((value >> 24) & 0xFF);
}

std::uint32_t ReadLe32(std::string_view bytes, std::size_t at) {
  const auto b0 = static_cast<unsigned char>(bytes[at]);
  const auto b1 = static_cast<unsigned char>(bytes[at + 1]);
  const auto b2 = static_cast<unsigned char>(bytes[at + 2]);
  const auto b3 = static_cast<unsigned char>(bytes[at + 3]);
  return static_cast<std::uint32_t>(b0) | (static_cast<std::uint32_t>(b1) << 8) |
         (static_cast<std::uint32_t>(b2) << 16) | (static_cast<std::uint32_t>(b3) << 24);
}

}  // namespace

namespace {
bool IsHello(MsgType type) {
  return type == MsgType::ClientHello || type == MsgType::PinyinHello || type == MsgType::PinyinHelloWithRaw;
}

bool ValidSize(MsgType type, std::size_t body) {
  switch (type) {
    case MsgType::StartListen: case MsgType::Cancel: case MsgType::Enter:
    case MsgType::ClientHello: case MsgType::PinyinHello: case MsgType::PinyinHelloWithRaw: return body == 8;
    case MsgType::PinyinKey: return body == 2;
    case MsgType::PinyinClear: case MsgType::PinyinInserted: return body == 0;
    case MsgType::PinyinView: return body >= 9;
    case MsgType::QueryCaret: case MsgType::ShowSettings: case MsgType::ServerReady:
    case MsgType::PreloadModel: return body == 0;
    case MsgType::Caret: return body == 32;
    case MsgType::Phase: return body == 9;
    case MsgType::InsertText: return body > 8;
    case MsgType::InsertResult: return body == 10 || body == 12;
    case MsgType::CancelInsert: return body == 8;
    case MsgType::PersonalTermUsed: return body>0 && body<=4096;
    case MsgType::Invalid: return false;
  }
  return false;
}
}

std::string Encode(const Message& message) {
  if (message.text.size() > kMaxFrameLength - 9) return {};
  std::string body;
  const bool hello = IsHello(message.type);
  if (IsVoiceControl(message.type) || message.type == MsgType::Phase || hello) {
    const auto value = hello ? message.client_window : message.state_id;
    if (!value) return {};
    body.assign(8, '\0');
    WriteLe32(body, 0, static_cast<std::uint32_t>(value));
    WriteLe32(body, 4, static_cast<std::uint32_t>(value >> 32));
    body += message.text;
  } else if (message.type == MsgType::Caret) {
    body.assign(32, '\0');
    const int values[] = {message.caret_x, message.caret_y, message.caret_w, message.caret_h,
                           message.window_x, message.window_y, message.window_w, message.window_h};
    for (int i = 0; i < 8; ++i) WriteLe32(body, static_cast<std::size_t>(i) * 4, static_cast<std::uint32_t>(values[i]));
  } else if (message.type == MsgType::InsertText || message.type == MsgType::InsertResult || message.type == MsgType::CancelInsert) {
    if (message.request_id == 0) return {};
    body.assign(8, '\0');
    WriteLe32(body, 0, static_cast<std::uint32_t>(message.request_id));
    WriteLe32(body, 4, static_cast<std::uint32_t>(message.request_id >> 32));
    body += message.text;
  } else {
    body = message.text;
  }
  if (!ValidSize(message.type, body.size())) return {};
  if (message.type == MsgType::InsertResult &&
      message.text != "ok" && message.text != "fail") return {};
  if (message.type == MsgType::Phase && (message.text[0] < '0' || message.text[0] > '5')) return {};
  const auto length = static_cast<std::uint32_t>(1 + body.size());
  std::string out(4 + length, '\0');
  WriteLe32(out, 0, length);
  out[4] = static_cast<char>(static_cast<std::uint8_t>(message.type));
  if (!body.empty()) std::memcpy(&out[5], body.data(), body.size());
  return out;
}

DecodeStatus DecodeFrame(std::string_view bytes, Message* out, std::size_t* consumed) {
  if (consumed != nullptr) *consumed = 0;
  if (bytes.size() < 4) return DecodeStatus::Incomplete;
  const std::uint32_t length = ReadLe32(bytes, 0);
  if (length < 1 || length > kMaxFrameLength) return DecodeStatus::Invalid;
  if (bytes.size() < 5) return DecodeStatus::Incomplete;
  Message message;
  message.type = static_cast<MsgType>(static_cast<unsigned char>(bytes[4]));
  if (!ValidSize(message.type, length - 1)) return DecodeStatus::Invalid;
  if (bytes.size() - 4 < length) return DecodeStatus::Incomplete;
  const std::string_view body = bytes.substr(5, length - 1);
  const bool hello = IsHello(message.type);
  if (IsVoiceControl(message.type) || message.type == MsgType::Phase || hello) {
    const auto value = ReadLe32(body, 0) | (static_cast<std::uint64_t>(ReadLe32(body, 4)) << 32);
    if (!value) return DecodeStatus::Invalid;
    if (hello) message.client_window = value;
    else message.state_id = value;
    message.text.assign(body.substr(8));
  } else if (message.type == MsgType::Caret) {
    int* values[] = {&message.caret_x, &message.caret_y, &message.caret_w, &message.caret_h,
                      &message.window_x, &message.window_y, &message.window_w, &message.window_h};
    for (int i = 0; i < 8; ++i) *values[i] = static_cast<int>(ReadLe32(body, static_cast<std::size_t>(i) * 4));
  } else if (message.type == MsgType::InsertText || message.type == MsgType::InsertResult || message.type == MsgType::CancelInsert) {
    message.request_id = ReadLe32(body, 0) | (static_cast<std::uint64_t>(ReadLe32(body, 4)) << 32);
    if (message.request_id == 0) return DecodeStatus::Invalid;
    message.text = std::string(body.substr(8));
  } else {
    message.text.assign(body.data(), body.size());
  }
  if (message.type == MsgType::InsertResult &&
      message.text != "ok" && message.text != "fail") return DecodeStatus::Invalid;
  if (message.type == MsgType::Phase && (message.text[0] < '0' || message.text[0] > '5')) return DecodeStatus::Invalid;
  if (out != nullptr) *out = std::move(message);
  if (consumed != nullptr) *consumed = 4 + static_cast<std::size_t>(length);
  return DecodeStatus::Complete;
}

bool Decode(std::string_view bytes, Message* out, std::size_t* consumed) {
  return DecodeFrame(bytes, out, consumed) == DecodeStatus::Complete;
}

bool IsVoiceControl(MsgType type) {
  return type == MsgType::StartListen || type == MsgType::Enter || type == MsgType::Cancel;
}
bool IsClientMessage(MsgType type) {
  return IsVoiceControl(type) || type == MsgType::Caret || type == MsgType::ShowSettings ||
      type == MsgType::PersonalTermUsed || type == MsgType::InsertResult || type == MsgType::PreloadModel;
}
