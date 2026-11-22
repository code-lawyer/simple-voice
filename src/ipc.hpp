#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

enum class MsgType : std::uint8_t {
  Invalid = 0,
  StartListen = 1,
  Cancel = 2,
  // 3–8 and 13 are retired. Never reuse their wire numbers.
  Enter = 9,
  QueryCaret = 10,
  Caret = 11,
  Phase = 12,
  ShowSettings = 14,
  // Versioned types: old DLLs must never interpret an ID as transcript text.
  InsertText = 15,
  InsertResult = 16,
  CancelInsert = 17,
  PersonalTermUsed = 18,
  ClientHello = 19,
  ServerReady = 20,
  PreloadModel = 21,
  PinyinHello = 22,
  PinyinKey = 23,
  PinyinClear = 24,
  PinyinView = 25,
  PinyinInserted = 26,
};

struct Message {
  MsgType type = MsgType::Invalid;
  std::string text;
  std::uint64_t request_id = 0;
  std::uint64_t state_id = 0;       // Phase snapshot / expected state for a voice command.
  std::uint64_t client_window = 0;  // Verified against the pipe peer PID and module.
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
bool IsVoiceControl(MsgType type);
bool IsClientMessage(MsgType type);
