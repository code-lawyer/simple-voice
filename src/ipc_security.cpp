#include "ipc_security.hpp"
#include <sddl.h>
#include <psapi.h>
#include <vector>

namespace {
struct Handle {
  HANDLE value = nullptr;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring SidText(PSID sid) {
  LPWSTR text = nullptr;
  if (!IsValidSid(sid) || !ConvertSidToStringSidW(sid, &text)) return {};
  const std::wstring result(text);
  LocalFree(text);
  return result;
}
struct Identity { std::wstring user, logon; DWORD session = 0; };
bool ProcessIdentity(HANDLE process, Identity& identity) {
  Handle token;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
  DWORD size = 0, container = 0;
  if (!GetTokenInformation(token.value, TokenIsAppContainer, &container, sizeof(container), &size) || container)
    return false;
  if (!GetTokenInformation(token.value, TokenSessionId, &identity.session, sizeof(identity.session), &size)) return false;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> user(size);
  if (!size || !GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) return false;
  identity.user = SidText(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
  GetTokenInformation(token.value, TokenGroups, nullptr, 0, &size);
  std::vector<BYTE> groups(size);
  if (!size || !GetTokenInformation(token.value, TokenGroups, groups.data(), size, &size)) return false;
  const auto* list = reinterpret_cast<const TOKEN_GROUPS*>(groups.data());
  for (DWORD i = 0; i < list->GroupCount; ++i) {
    if ((list->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
      identity.logon = SidText(list->Groups[i].Sid);
  }
  return !identity.user.empty() && !identity.logon.empty();
}
std::shared_ptr<IpcPeer> Peer(HANDLE pipe, const char* query_name) {
  using Query = BOOL (WINAPI*)(HANDLE, PULONG);
  const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), query_name));
  DWORD id = 0;
  if (!query || !query(pipe, &id) || !id) return {};
  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, id);
  if (!process) return {};
  auto peer = std::make_shared<IpcPeer>(process, id);
  Identity own, remote;
  if (!ProcessIdentity(GetCurrentProcess(), own) || !ProcessIdentity(process, remote) ||
      own.user != remote.user || own.logon != remote.logon || own.session != remote.session || !peer->alive()) return {};
  return peer;
}
std::wstring ScopeName(const wchar_t* prefix) {
  Identity identity;
  return ProcessIdentity(GetCurrentProcess(), identity) ? std::wstring(prefix) + identity.logon : L"";
}
}  // namespace

std::wstring VoicePipeName() { return ScopeName(L"\\\\.\\pipe\\SimpleVoice.v2."); }
std::wstring VoiceMutexName() { return ScopeName(L"Local\\SimpleVoice.v2."); }
std::wstring RegisteredVoiceDll() {
  const wchar_t* key=L"Software\\Classes\\CLSID\\{A4C1E2B0-7D34-4F91-9A62-5E18C3704B21}\\InProcServer32";
  for(HKEY hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}) {
    HKEY handle=nullptr;
    if(RegOpenKeyExW(hive,key,0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&handle)!=ERROR_SUCCESS)continue;
    wchar_t value[32768]{};DWORD bytes=sizeof(value),type=0;
    const auto status=RegQueryValueExW(handle,nullptr,nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes);RegCloseKey(handle);
    if(status==ERROR_SUCCESS && type==REG_SZ && bytes>=sizeof(wchar_t) && bytes<=sizeof(value) && value[bytes/sizeof(wchar_t)-1]==0)
      return std::wstring(value);
  }
  return {};
}
bool LegacyVoiceEngineRunning() {
  HANDLE old=OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\VoiceImeEngine");
  if(old){CloseHandle(old);return true;}return GetLastError()==ERROR_ACCESS_DENIED;
}
bool SameFile(const std::wstring& first, const std::wstring& second) {
  if (first.empty() || second.empty()) return false;
  Handle a{CreateFileW(first.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, 0, nullptr)};
  Handle b{CreateFileW(second.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, 0, nullptr)};
  BY_HANDLE_FILE_INFORMATION left{}, right{};
  return a.value != INVALID_HANDLE_VALUE && b.value != INVALID_HANDLE_VALUE &&
      GetFileInformationByHandle(a.value, &left) && GetFileInformationByHandle(b.value, &right) &&
      left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
      left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
}
PipeSecurity::PipeSecurity() {
  Identity identity;
  if (!ProcessIdentity(GetCurrentProcess(), identity)) return;
  const std::wstring sddl = L"O:" + identity.user + L"D:P(A;;GA;;;" + identity.logon + L")";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) return;
  attributes_ = {sizeof(SECURITY_ATTRIBUTES), descriptor_, FALSE};
}
PipeSecurity::~PipeSecurity() { if (descriptor_) LocalFree(descriptor_); }
SECURITY_ATTRIBUTES* PipeSecurity::attributes() { return descriptor_ ? &attributes_ : nullptr; }
IpcPeer::~IpcPeer() { if (process_) CloseHandle(process_); }
bool IpcPeer::alive() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }
bool IpcPeer::owns_window(HWND window, const std::wstring& expected_module) const {
  DWORD owner = 0;
  if (!alive() || !IsWindow(window) || !GetWindowThreadProcessId(window, &owner) || owner != id_) return false;
  wchar_t name[80]{};
  if (!GetClassNameW(window, name, 80) || lstrcmpW(name, L"VoiceImeTsfHost") != 0) return false;
  const auto module = reinterpret_cast<HMODULE>(GetClassLongPtrW(window, GCLP_HMODULE));
  if (!module) return false;
  std::vector<wchar_t> path(32768);
  const DWORD size = GetModuleFileNameExW(process_, module, path.data(), static_cast<DWORD>(path.size()));
  return size && size < path.size() && SameFile(std::wstring(path.data(), size), expected_module);
}
bool IpcPeer::is_image(const std::wstring& expected_image) const {
  if (!alive()) return false;
  std::vector<wchar_t> path(32768);
  DWORD size = static_cast<DWORD>(path.size());
  return QueryFullProcessImageNameW(process_, 0, path.data(), &size) && SameFile(std::wstring(path.data(), size), expected_image);
}
std::shared_ptr<IpcPeer> PipeClientPeer(HANDLE pipe) { return Peer(pipe, "GetNamedPipeClientProcessId"); }
std::shared_ptr<IpcPeer> PipeServerPeer(HANDLE pipe) { return Peer(pipe, "GetNamedPipeServerProcessId"); }
DWORD ForegroundProcessId() {
  DWORD process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process);
  return process;
}

PipeDeadline::PipeDeadline(DWORD milliseconds) {
  timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
  LARGE_INTEGER due{}; due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000;
  if (timer_ && !SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
    CloseHandle(timer_); timer_ = nullptr;
  }
}
PipeDeadline::~PipeDeadline() { if (timer_) CloseHandle(timer_); }
bool ClientHandshake(HANDLE pipe, HWND window, HANDLE stop, Message& initial, std::string& remaining) {
  PipeDeadline deadline;
  if (!deadline.event()) return false;
  Message hello; hello.type = MsgType::ClientHello;
  hello.client_window = reinterpret_cast<std::uintptr_t>(window);
  const auto bytes = Encode(hello);
  if (bytes.empty() || !WritePipeBytes(pipe, bytes, deadline.event())) return false;
  bool ready = false;
  remaining.clear();
  for (;;) {
    Message message; std::size_t used = 0;
    const auto status = DecodeFrame(remaining, &message, &used);
    if (status == DecodeStatus::Invalid) return false;
    if (status == DecodeStatus::Complete) {
      remaining.erase(0, used);
      if (!ready) {
        if (message.type != MsgType::ServerReady) return false;
        ready = true;
      } else {
        if (message.type != MsgType::Phase) return false;
        initial = std::move(message);
        return true;
      }
      continue;
    }
    const auto read = ReadPipeChunk(pipe, deadline.event(), stop);
    remaining += read.bytes;
    if (read.status != PipeReadStatus::Data) return false;
  }
}
