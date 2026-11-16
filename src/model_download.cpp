#include "model_download.hpp"
#include "utf8.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>

namespace {
std::wstring StatusPath(const std::wstring& directory) {return directory+L"\\.downloads\\model-status.txt";}
std::wstring Quote(const std::wstring& value) {
  // File-system paths cannot contain quotes; do not pass a shell command.
  std::wstring result=L"\"";size_t slashes=0;
  for(wchar_t c:value) {if(c==L'\\'){++slashes;continue;} result.append(slashes,L'\\');slashes=0;result+=c;}
  result.append(slashes*2,L'\\');return result+L"\"";
}
}
std::wstring ModelDownloadStatus(const std::wstring& directory) {
  std::ifstream file(std::filesystem::path(StatusPath(directory)),std::ios::binary);
  std::string data(4096,'\0');file.read(data.data(),data.size());data.resize(static_cast<size_t>(file.gcount()));
  if(data.substr(0,3)=="\xef\xbb\xbf")data.erase(0,3);
  return data.empty()?L"正在下载并校验模型…":Utf8ToWide(data);
}
bool DownloadModel(const std::wstring& program,const std::wstring& directory,SpeechModel model,
                   const std::atomic<bool>& cancelled,std::wstring& message) {
  const char* id=SpeechModelId(model);if(!id){message=L"未知模型。";return false;}
  std::error_code error;std::filesystem::create_directories(std::filesystem::path(directory)/L".downloads",error);
  if(error){message=L"无法写入模型下载目录。";return false;}
  const auto status=StatusPath(directory);std::ofstream(std::filesystem::path(status),std::ios::trunc)<<"正在准备下载…";
  wchar_t system[MAX_PATH];if(!GetSystemDirectoryW(system,MAX_PATH)){message=L"无法定位系统下载工具。";return false;}
  const std::wstring powershell=std::wstring(system)+L"\\WindowsPowerShell\\v1.0\\powershell.exe";
  const std::wstring script=program+L"\\tools\\download_models.ps1";
  std::wstring command=Quote(powershell)+L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+Quote(script)+
      L" -Root "+Quote(directory)+L" -OnlyModel "+Utf8ToWide(id)+L" -StatusFile "+Quote(status);
  HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if(!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) {
    if(job)CloseHandle(job);message=L"无法准备下载任务。";return false;
  }
  STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
  if(!CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,
                    nullptr,program.c_str(),&startup,&process)) {
    CloseHandle(job);message=L"无法启动下载。请检查安装包是否完整。";return false;
  }
  if(!AssignProcessToJobObject(job,process.hProcess)) {
    TerminateProcess(process.hProcess,1);CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(job);
    message=L"无法创建可取消的下载任务。";return false;
  }
  ResumeThread(process.hThread);CloseHandle(process.hThread);
  const auto deadline=GetTickCount64()+45*60*1000;
  while(WaitForSingleObject(process.hProcess,100)==WAIT_TIMEOUT && !cancelled && GetTickCount64()<deadline) {}
  DWORD code=1;
  if(cancelled || GetTickCount64()>=deadline) {
    TerminateJobObject(job,1);WaitForSingleObject(process.hProcess,5000);
    message=cancelled?L"下载已取消。已验证的模型保留，可以重试。":L"下载超时，可以重试。";
  } else {
    GetExitCodeProcess(process.hProcess,&code);message=ModelDownloadStatus(directory);
    if(code!=0 && message.rfind(L"下载失败",0)!=0)message=L"下载失败，可以重试。\r\n"+message;
  }
  CloseHandle(process.hProcess);CloseHandle(job);
  return code==0 && !cancelled;
}
