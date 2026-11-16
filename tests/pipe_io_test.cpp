#include "harness.hpp"
#include "ipc.hpp"
#include "pipe_io.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace {
struct PipePair {
  HANDLE server = INVALID_HANDLE_VALUE;
  HANDLE client = INVALID_HANDLE_VALUE;
  HANDLE wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  PipePair() {
    static std::atomic<int> serial{0};
    const auto name = L"\\\\.\\pipe\\VoiceImeTest_" + std::to_wstring(GetCurrentProcessId()) +
        L"_" + std::to_wstring(++serial);
    server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    if (server == INVALID_HANDLE_VALUE) return;
    OVERLAPPED connect{};
    connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ConnectNamedPipe(server, &connect);
    client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    DWORD unused = 0;
    if (client == INVALID_HANDLE_VALUE) CancelIoEx(server, &connect);
    GetOverlappedResult(server, &connect, &unused, TRUE);
    CloseHandle(connect.hEvent);
  }
  ~PipePair() {
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE) { DisconnectNamedPipe(server); CloseHandle(server); }
    CloseHandle(wake);
    CloseHandle(stop);
  }
  bool valid() const { return server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE; }
};
}  // namespace

void PipeReadSurvivesRepeatedTimeouts() {
  PipePair pipe;
  CHECK(pipe.valid());
  if (!pipe.valid()) return;
  Message message;
  message.type = MsgType::InsertText; message.request_id = 1;
  message.text = "超时之后仍然准确上屏";
  const auto frame = Encode(message);
  bool wrote = false;
  std::thread writer([&] {
    Sleep(30);
    wrote = WritePipeBytes(pipe.client, frame.substr(0, 3), pipe.stop);
    Sleep(30);
    wrote = WritePipeBytes(pipe.client, frame.substr(3), pipe.stop) && wrote;
    Sleep(30);
    SetEvent(pipe.stop);
  });
  std::string bytes;
  while (WaitForSingleObject(pipe.stop, 0) != WAIT_OBJECT_0 && bytes.size() < frame.size()) {
    const auto read = ReadPipeChunk(pipe.server, pipe.wake, pipe.stop, 2);
    bytes += read.bytes;
    if (read.status == PipeReadStatus::Closed) break;
  }
  writer.join();
  CHECK(wrote);
  CHECK(bytes == frame);
  Message decoded;
  std::size_t consumed = 0;
  CHECK(Decode(bytes, &decoded, &consumed));
  CHECK(decoded.text == message.text);
}

void PipeWakeCancelsOnlyThePendingRead() {
  PipePair pipe;
  CHECK(pipe.valid());
  if (!pipe.valid()) return;
  std::thread wake([&] { Sleep(10); SetEvent(pipe.wake); });
  const auto interrupted = ReadPipeChunk(pipe.server, pipe.wake, pipe.stop, 2);
  wake.join();
  CHECK(interrupted.status == PipeReadStatus::Wake);
  CHECK(interrupted.bytes.empty());
  ResetEvent(pipe.wake);
  CHECK(WritePipeBytes(pipe.client, "after wake", pipe.stop));
  const auto next = ReadPipeChunk(pipe.server, pipe.wake, pipe.stop, 2);
  CHECK(next.bytes == "after wake");
}

void PipeWakeRacingWithDataLosesNoBytes() {
  PipePair pipe;
  CHECK(pipe.valid());
  if (!pipe.valid()) return;
  for (int n = 0; n < 40; ++n) {
    ResetEvent(pipe.wake);
    const auto expected = "frame-" + std::to_string(n);
    bool wrote = false;
    std::thread writer([&] {
      wrote = WritePipeBytes(pipe.client, expected, pipe.stop);
      SetEvent(pipe.wake);
    });
    auto read = ReadPipeChunk(pipe.server, pipe.wake, pipe.stop, 1);
    writer.join();
    CHECK(wrote);
    ResetEvent(pipe.wake);
    if (read.bytes.empty()) read = ReadPipeChunk(pipe.server, pipe.wake, pipe.stop, 1);
    CHECK(read.bytes == expected);
  }
}

void PipeDisconnectFinishesPendingRead() {
  PipePair pipe;
  CHECK(pipe.valid());
  if (!pipe.valid()) return;
  const HANDLE client = pipe.client;
  pipe.client = INVALID_HANDLE_VALUE;
  std::thread closer([client] { Sleep(10); CloseHandle(client); });
  const auto read = ReadPipeChunk(pipe.server, nullptr, pipe.stop, 2);
  closer.join();
  CHECK(read.status == PipeReadStatus::Closed);
}

void PipeStopCancelsBlockedWrite() {
  PipePair pipe;
  CHECK(pipe.valid());
  if (!pipe.valid()) return;
  std::thread stopper([&] { Sleep(15); SetEvent(pipe.stop); });
  const auto started = std::chrono::steady_clock::now();
  const bool written = WritePipeBytes(pipe.client, std::string(2 * 1024 * 1024, 'x'), pipe.stop);
  stopper.join();
  CHECK(!written);
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

void PipeStopFinishesPendingRead() {
  PipePair pipe;
  CHECK(pipe.valid());
  if (!pipe.valid()) return;
  std::thread stopper([&] { Sleep(10); SetEvent(pipe.stop); });
  const auto read = ReadPipeChunk(pipe.server, nullptr, pipe.stop, 2);
  stopper.join();
  CHECK(read.status == PipeReadStatus::Wake);
}
