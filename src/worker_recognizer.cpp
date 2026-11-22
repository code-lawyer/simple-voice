#include "worker_recognizer.hpp"
#include "worker_protocol.hpp"
#include "utf8.hpp"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <psapi.h>

namespace {
void Close(HANDLE& h) { if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h);h=nullptr; }
class WorkerRecognizer final:public Recognizer {
 public:
  ~WorkerRecognizer() override {
    Cancel(); if(process_) WaitForSingleObject(process_,3000);
    Close(input_);Close(output_);Close(reply_readable_);Close(final_ready_);Close(process_);Close(job_);
  }
  bool Start(const std::wstring& executable,const std::string& root,const ModelSettings& settings,
             unsigned timeout,std::string& error,const std::atomic<bool>* cancel_loading,ModelStartup startup_mode) {
    cancel_loading_=cancel_loading;
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE child_in=nullptr,child_out=nullptr,child_signal=nullptr,child_final=nullptr,discard=nullptr,thread=nullptr;
    std::vector<unsigned char> attributes;
    auto cleanup=[&] { Close(child_in);Close(child_out);Close(child_signal);Close(child_final);Close(discard);Close(thread); };
    reply_readable_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    final_ready_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!final_ready_ || !DuplicateHandle(GetCurrentProcess(),final_ready_,GetCurrentProcess(),&child_final,
        EVENT_MODIFY_STATE,TRUE,0) || !reply_readable_ || !DuplicateHandle(GetCurrentProcess(), reply_readable_, GetCurrentProcess(),
        &child_signal, EVENT_MODIFY_STATE, TRUE, 0) ||
        !CreatePipe(&child_in,&input_,&security,65536) || !CreatePipe(&output_,&child_out,&security,65536)) {
      cleanup();error="无法建立模型进程通信。";return false;
    }
    SetHandleInformation(input_,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output_,HANDLE_FLAG_INHERIT,0);
    discard=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    job_=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job_ || !SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) {
      cleanup();error="无法创建可取消的模型进程。";return false;
    }
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);attributes.resize(bytes);
    auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if(!InitializeProcThreadAttributeList(list,1,0,&bytes)) {cleanup();error="无法准备模型进程。";return false;}
    HANDLE handles[]={child_in,child_out,discard,child_signal,child_final};
    bool ok=UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr)!=FALSE;
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=list;
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=discard;
    startup.StartupInfo.hStdOutput=discard;startup.StartupInfo.hStdError=discard;
    std::wstring command=L"\""+executable+L"\" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(child_in))+
      L" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(child_out))+
      L" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(child_signal))+
      L" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(child_final));
    const auto directory=std::filesystem::path(executable).parent_path().wstring();
    PROCESS_INFORMATION process{};
    if(ok)ok=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,directory.c_str(),&startup.StartupInfo,&process)!=FALSE;
    DeleteProcThreadAttributeList(list);
    if(!ok) {cleanup();error="无法启动模型进程。请检查安装包是否完整。";return false;}
    process_=process.hProcess;thread=process.hThread;
    if(!AssignProcessToJobObject(job_,process_)) {
      TerminateProcess(process_,1);cleanup();error="无法隔离模型进程。";return false;
    }
    ResumeThread(thread);cleanup();
    try {
      const auto operation=startup_mode==ModelStartup::DraftFirst?model_worker::InitDraft:model_worker::Init;
      Request(operation,model_worker::Fields({root,SerializeModelSettings(settings)}),timeout);
      cancel_loading_=nullptr;ready_=true;return true;
    } catch(const std::exception& e) {error=e.what();return false;}
  }
  void Cancel() override {cancelled_=true; if(job_)TerminateJobObject(job_,1);}
  bool Healthy() const override {
    return ready_ && !cancelled_ && process_ && WaitForSingleObject(process_,0)==WAIT_TIMEOUT;
  }
  bool FinalReady() const override {return Healthy() && WaitForSingleObject(final_ready_,0)==WAIT_OBJECT_0;}
  std::size_t PeakMemoryBytes() const override {
    PROCESS_MEMORY_COUNTERS memory{};
    return process_ && GetProcessMemoryInfo(process_,&memory,sizeof(memory))?memory.PeakWorkingSetSize:0;
  }
  bool AcceptAudio(const std::int16_t* samples,std::size_t count) override {
    if(!samples || !count)return false;
    bool changed=false;
    try {
      std::lock_guard<std::mutex> rpc_lock(rpc_mutex_);
      if(count>model_worker::kLimit/sizeof(*samples))throw std::runtime_error("音频片段过大。请重新录音。");
      // Every request fits in the pipe buffer even if the child stops reading.
      // The response deadline or cancellation then always remains reachable.
      while(count) {
        const auto chunk=(std::min)(count,size_t(16000));
        const auto response=Request(model_worker::Audio,std::string(reinterpret_cast<const char*>(samples),chunk*sizeof(*samples)),15000);
        {
          std::lock_guard<std::mutex> cache_lock(cache_mutex_);
          if(!response[0].empty() && partial_!=response[0]) {partial_=response[0];changed=true;}
        }
        samples+=chunk;count-=chunk;
      }
    } catch(const std::exception& e) {
      Cancel();std::lock_guard<std::mutex> cache_lock(cache_mutex_);warning_=e.what();
    }
    return changed;
  }
  std::string Partial() const override {std::lock_guard<std::mutex> lock(cache_mutex_);return partial_;}
  std::string Finalize() override {
    std::lock_guard<std::mutex> rpc_lock(rpc_mutex_);
    const auto response=Request(model_worker::Finalize,{},120000);
    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
    partial_=response[0];original_=response[1];warning_=response[2];return partial_;
  }
  void Reset() override {
    std::lock_guard<std::mutex> rpc_lock(rpc_mutex_);
    Request(model_worker::Reset,{},15000);
    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
    partial_.clear();original_.clear();warning_.clear();
  }
  std::string Original() const override {std::lock_guard<std::mutex> lock(cache_mutex_);return original_;}
  std::string TakeWarning() override {std::lock_guard<std::mutex> lock(cache_mutex_);std::string result;result.swap(warning_);return result;}
 private:
  bool ReadBefore(void* bytes,size_t count,ULONGLONG deadline) {
    auto* to=static_cast<char*>(bytes);
    while(count && !cancelled_ && !(cancel_loading_ && cancel_loading_->load())) {
      DWORD available=0;
      if(!PeekNamedPipe(output_,nullptr,0,nullptr,&available,nullptr))return false;
      if(available) {
        DWORD got=0;
        if(!ReadFile(output_,to,static_cast<DWORD>((std::min)(count,size_t(available))),&got,nullptr)||!got)return false;
        count-=got;to+=got;continue;
      }
      const auto now = GetTickCount64();
      if (now >= deadline) return false;
      auto wait = static_cast<DWORD>(deadline - now);
      // Only startup cancellation uses an external flag. Inference cancellation
      // terminates the worker, which wakes the process handle immediately.
      if (cancel_loading_) wait = (std::min)(wait, DWORD(25));
      const HANDLE signals[] = {reply_readable_, process_};
      const auto result = WaitForMultipleObjects(2, signals, FALSE, wait);
      if (result == WAIT_FAILED) return false;
      if (result == WAIT_OBJECT_0 + 1) {
        // A worker may write its error reply immediately before exiting.
        // Drain that reply instead of replacing it with a generic crash error.
        if(!PeekNamedPipe(output_,nullptr,0,nullptr,&available,nullptr) || !available)return false;
      }
    }
    return count==0 && !cancelled_ && !(cancel_loading_ && cancel_loading_->load());
  }
  std::vector<std::string> Request(std::uint32_t operation,const std::string& bytes,unsigned timeout) {
    auto fail=[&](const char* message)->void {Cancel();throw std::runtime_error(message);};
    if(cancelled_ || !model_worker::Packet(input_,operation,bytes))fail("模型进程已结束。请重试录音或重新准备模型。");
    const auto deadline=GetTickCount64()+timeout;
    model_worker::Header response{};
    if(!ReadBefore(&response,sizeof(response),deadline))fail("模型未能完成操作，已停止本次计算。请重试或更换模型。");
    if(response.magic!=model_worker::kMagic || response.size>model_worker::kLimit)fail("模型进程返回了无效数据。");
    std::string data(response.size,'\0');
    if(!ReadBefore(data.data(),data.size(),deadline))fail("模型进程通信中断，请重试。");
    std::vector<std::string> result;
    if(response.operation==model_worker::Error) {
      if(model_worker::Fields(data,1,result)) {Cancel();throw std::runtime_error(result[0]);}
      fail("模型无法加载，请选择对应型号的完整模型文件夹。");
    }
    if(response.operation!=operation || !model_worker::Fields(data,3,result))fail("模型进程返回格式不正确。");
    return result;
  }
  HANDLE process_=nullptr,job_=nullptr,input_=nullptr,output_=nullptr;
  HANDLE reply_readable_=nullptr,final_ready_=nullptr;
  std::atomic<bool> cancelled_{false};
  const std::atomic<bool>* cancel_loading_=nullptr;
  bool ready_=false;
  // Slow requests serialize independently from the text read by the UI thread.
  std::mutex rpc_mutex_;
  mutable std::mutex cache_mutex_;
  std::string partial_,original_,warning_;
};
}
std::unique_ptr<Recognizer> MakeWorkerRecognizer(const std::string& root,const ModelSettings& settings,
    std::string* error,const std::wstring& executable,unsigned timeout,const std::atomic<bool>* cancel_loading,ModelStartup startup_mode) {
  ModelFiles files;std::string message;
  std::error_code path_error;
  const auto absolute_root=std::filesystem::absolute(std::filesystem::path(Utf8ToWide(root)),path_error);
  if(path_error){if(error)*error="无法定位模型目录。";return nullptr;}
  auto normalized=settings;
  for(auto& directory:normalized.directories)if(!directory.empty()) {
    directory=std::filesystem::absolute(std::filesystem::path(directory),path_error).wstring();
    if(path_error){if(error)*error="模型目录无效。";return nullptr;}
  }
  const auto absolute_utf8=WideToUtf8(absolute_root.wstring());
  if(!ResolveModelFiles(absolute_utf8,normalized,files,message)) {if(error)*error=message;return nullptr;}
  std::wstring path=executable;
  if(path.empty()) {
    wchar_t own[32768];const auto count=GetModuleFileNameW(nullptr,own,32768);
    if(!count || count>=32768) {if(error)*error="无法定位模型进程。";return nullptr;}
    path=(std::filesystem::path(std::wstring(own,count)).parent_path()/L"voiceime_model_worker.exe").wstring();
  }
  auto worker=std::make_unique<WorkerRecognizer>();
  if(!worker->Start(path,absolute_utf8,normalized,timeout,message,cancel_loading,startup_mode)) {if(error)*error=message;return nullptr;}
  if(error)error->clear();return worker;
}
