#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

enum class MsgType : std::uint8_t {
  StartListen = 1,
  Cancel = 2,
  Draft = 3,
  Final = 4,
  CommitDone = 5,
  ListRecent = 6,
  DeleteEntry = 7,
  SetHotkey = 8,
  Enter = 9,
  QueryCaret = 10,
  Caret = 11,
  Phase = 12,
  InsertAck = 13,
  ShowSettings = 14,
};

struct Message {
  MsgType type = MsgType::Draft;
  std::string text;
  int key_mod = 0;
  int key_vk = 0;
  int caret_x = 0;
  int caret_y = 0;
  int caret_w = 0;
  int caret_h = 0;
  int window_x = 0;
  int window_y = 0;
  int window_w = 0;
  int window_h = 0;
};

std::string Encode(const Message& message);
bool Decode(std::string_view bytes, Message* out, std::size_t* consumed);
