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
  // Versioned types: old DLLs must never interpret an ID as transcript text.
  InsertText = 15,
  InsertResult = 16,
  CancelInsert = 17,
};

struct Message {
  MsgType type = MsgType::Draft;
  std::string text;
  std::uint64_t request_id = 0;
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
constexpr std::size_t kMaxFrameLength = 64 * 1024;  // type + payload, excluding length prefix
enum class DecodeStatus { Incomplete, Complete, Invalid };
DecodeStatus DecodeFrame(std::string_view bytes, Message* out, std::size_t* consumed);
bool Decode(std::string_view bytes, Message* out, std::size_t* consumed);
