#include "harness.hpp"
#include "ipc_security.hpp"
#include <sddl.h>

void PipePeersAndLogonAclAreVerified() {
  PipeSecurity security;
  CHECK(security.attributes()!=nullptr);
  if(!security.attributes())return;
  BOOL present=FALSE,defaulted=FALSE; PACL acl=nullptr;
  CHECK(GetSecurityDescriptorDacl(security.attributes()->lpSecurityDescriptor,&present,&acl,&defaulted));
  CHECK(present && acl && acl->AceCount==1);
  LPWSTR sddl=nullptr;
  CHECK(ConvertSecurityDescriptorToStringSecurityDescriptorW(security.attributes()->lpSecurityDescriptor,
        SDDL_REVISION_1,DACL_SECURITY_INFORMATION,&sddl,nullptr));
  if(sddl) {
    const std::wstring value(sddl);
    CHECK(value.find(L"S-1-5-5-")!=std::wstring::npos); // Windows logon SID, not Everyone.
    CHECK(value.find(L";;;WD)")==std::wstring::npos);
    LocalFree(sddl);
  }
  CHECK(VoicePipeName().find(L"SimpleVoice.v2.S-1-5-5-")!=std::wstring::npos);
  const auto name=L"\\\\.\\pipe\\VoiceImeSecurityQA_"+std::to_wstring(GetCurrentProcessId());
  HANDLE server=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,security.attributes());
  CHECK(server!=INVALID_HANDLE_VALUE);
  if(server==INVALID_HANDLE_VALUE)return;
  HANDLE client=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
  CHECK(client!=INVALID_HANDLE_VALUE);
  if(client==INVALID_HANDLE_VALUE){CloseHandle(server);return;}
  const auto remote_client=PipeClientPeer(server),remote_server=PipeServerPeer(client);
  CHECK(remote_client && remote_server);
  wchar_t image[32768]{}; GetModuleFileNameW(nullptr,image,32768);
  WNDCLASSW klass{}; klass.lpfnWndProc=DefWindowProcW; klass.hInstance=GetModuleHandleW(nullptr);
  klass.lpszClassName=L"VoiceImeTsfHost"; RegisterClassW(&klass);
  HWND window=CreateWindowW(klass.lpszClassName,L"",WS_POPUP,0,0,0,0,nullptr,nullptr,klass.hInstance,nullptr);
  CHECK(window!=nullptr);
  if(remote_client && remote_server) {
    CHECK(remote_client->id()==GetCurrentProcessId());
    CHECK(remote_server->is_image(image));
    CHECK(!remote_server->is_image(ExecutableDirectory()+L"\\voiceime_engine.exe"));
    CHECK(remote_client->owns_window(window,image));
    CHECK(!remote_client->owns_window(window,ExecutableDirectory()+L"\\voiceime_tsf.dll"));
    CHECK(!remote_client->owns_window(nullptr,image));
    const auto destroyed=window;
    DestroyWindow(window); window=nullptr;
    CHECK(!remote_client->owns_window(destroyed,image));
  }
  if(window)DestroyWindow(window);
  CloseHandle(client); DisconnectNamedPipe(server); CloseHandle(server);
}
