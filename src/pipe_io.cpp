#include "pipe_io.hpp"

namespace {

class Operation {
 public:
  Operation() { value.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~Operation() { if (value.hEvent != nullptr) CloseHandle(value.hEvent); }
  OVERLAPPED value{};
};

bool Stopped(HANDLE stop) {
  return stop != nullptr && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0;
}

// CancelIoEx does not wait for completion. Drain before releasing the buffer,
// event, or OVERLAPPED, including when completion won the cancellation race.
BOOL CancelAndDrain(HANDLE pipe, OVERLAPPED* operation, DWORD* transferred) {
  CancelIoEx(pipe, operation);
  return GetOverlappedResult(pipe, operation, transferred, TRUE);
}

}  // namespace

PipeReadResult ReadPipeChunk(HANDLE pipe, HANDLE wake_event, HANDLE stop_event, DWORD poll_ms) {
  if (Stopped(stop_event)) return {PipeReadStatus::Wake, {}};
  Operation operation;
  if (operation.value.hEvent == nullptr) return {};
  char chunk[4096];
  DWORD got = 0;
  BOOL ok = ReadFile(pipe, chunk, sizeof(chunk), &got, &operation.value);
  if (!ok && GetLastError() != ERROR_IO_PENDING) return {};
  if (ok) return {got > 0 ? PipeReadStatus::Data : PipeReadStatus::Closed, std::string(chunk, got)};

  HANDLE waits[3] = {operation.value.hEvent};
  DWORD count = 1;
  if (wake_event != nullptr) waits[count++] = wake_event;
  if (stop_event != nullptr) waits[count++] = stop_event;
  for (;;) {
    const DWORD which = WaitForMultipleObjects(count, waits, FALSE, poll_ms);
    if (which == WAIT_TIMEOUT) {
      // Timeout is not completion. Keep waiting for this exact read.
      DWORD available = 0;
      if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) continue;
      ok = CancelAndDrain(pipe, &operation.value, &got);
      return {PipeReadStatus::Closed, ok ? std::string(chunk, got) : std::string()};
    }
    if (which == WAIT_OBJECT_0) {
      ok = GetOverlappedResult(pipe, &operation.value, &got, FALSE);
      if (!ok || got == 0) return {};
      return {PipeReadStatus::Data, std::string(chunk, got)};
    }
    ok = CancelAndDrain(pipe, &operation.value, &got);
    return {which == WAIT_FAILED ? PipeReadStatus::Closed : PipeReadStatus::Wake,
            ok ? std::string(chunk, got) : std::string()};
  }
}

bool WritePipeBytes(HANDLE pipe, const std::string& bytes, HANDLE stop_event) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (Stopped(stop_event)) return false;
    Operation operation;
    if (operation.value.hEvent == nullptr) return false;
    DWORD wrote = 0;
    const DWORD size = static_cast<DWORD>((bytes.size() - offset) > MAXDWORD
        ? MAXDWORD : bytes.size() - offset);
    BOOL ok = WriteFile(pipe, bytes.data() + offset, size, &wrote, &operation.value);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      HANDLE waits[2] = {operation.value.hEvent, stop_event};
      const DWORD which = WaitForMultipleObjects(stop_event == nullptr ? 1 : 2, waits, FALSE, INFINITE);
      if (which == WAIT_OBJECT_0) ok = GetOverlappedResult(pipe, &operation.value, &wrote, FALSE);
      else {
        CancelAndDrain(pipe, &operation.value, &wrote);
        return false;
      }
    }
    if (!ok || wrote == 0) return false;
    offset += wrote;
  }
  return true;
}
