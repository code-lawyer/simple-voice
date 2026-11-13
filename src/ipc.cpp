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

std::string Encode(const Message& message) {
  std::string body;
  if (message.type == MsgType::SetHotkey) {
    body.assign(8, '\0');
    WriteLe32(body, 0, static_cast<std::uint32_t>(message.key_mod));
    WriteLe32(body, 4, static_cast<std::uint32_t>(message.key_vk));
  } else if (message.type == MsgType::Caret) {
    body.assign(32, '\0');
    const int values[] = {message.caret_x, message.caret_y, message.caret_w, message.caret_h,
                           message.window_x, message.window_y, message.window_w, message.window_h};
    for (int i = 0; i < 8; ++i) WriteLe32(body, static_cast<std::size_t>(i) * 4, static_cast<std::uint32_t>(values[i]));
  } else {
    body = message.text;
  }
  const auto length = static_cast<std::uint32_t>(1 + body.size());
  std::string out(4 + length, '\0');
  WriteLe32(out, 0, length);
  out[4] = static_cast<char>(static_cast<std::uint8_t>(message.type));
  if (!body.empty()) std::memcpy(&out[5], body.data(), body.size());
  return out;
}

bool Decode(std::string_view bytes, Message* out, std::size_t* consumed) {
  if (consumed != nullptr) *consumed = 0;
  if (bytes.size() < 4) return false;
  const std::uint32_t length = ReadLe32(bytes, 0);
  if (length < 1 || bytes.size() < 4u + length) return false;
  Message message;
  message.type = static_cast<MsgType>(static_cast<unsigned char>(bytes[4]));
  const std::string_view body = bytes.substr(5, length - 1);
  if (message.type == MsgType::SetHotkey) {
    if (body.size() != 8) return false;
    message.key_mod = static_cast<int>(ReadLe32(body, 0));
    message.key_vk = static_cast<int>(ReadLe32(body, 4));
  } else if (message.type == MsgType::Caret) {
    if (body.size() != 32) return false;
    int* values[] = {&message.caret_x, &message.caret_y, &message.caret_w, &message.caret_h,
                      &message.window_x, &message.window_y, &message.window_w, &message.window_h};
    for (int i = 0; i < 8; ++i) *values[i] = static_cast<int>(ReadLe32(body, static_cast<std::size_t>(i) * 4));
  } else {
    message.text.assign(body.data(), body.size());
  }
  if (out != nullptr) *out = std::move(message);
  if (consumed != nullptr) *consumed = 4u + length;
  return true;
}
