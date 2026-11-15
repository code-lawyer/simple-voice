#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

enum class PipeReadStatus { Data, Wake, Closed };

// A wake/cancel may race with a successful read. Always deliver bytes, even
// when status is Wake. The operation is completed before this function returns.
struct PipeReadResult {
  PipeReadStatus status = PipeReadStatus::Closed;
  std::string bytes;
};

PipeReadResult ReadPipeChunk(HANDLE pipe, HANDLE wake_event, HANDLE stop_event,
                             DWORD poll_ms = 1000);
bool WritePipeBytes(HANDLE pipe, const std::string& bytes, HANDLE stop_event = nullptr);
