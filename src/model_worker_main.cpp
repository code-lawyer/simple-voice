#include "worker_protocol.hpp"
#include "sherpa_recognizer.hpp"
#include "model_settings.hpp"
#include "recording.hpp"
#include <cstdlib>
#include <memory>
#include <stdexcept>

int main(int argc,char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  if(argc<3 || argc>5)return 2;
  const auto input=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(argv[1],nullptr,10)));
  const auto output=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(argv[2],nullptr,10)));
  if(GetFileType(input)!=FILE_TYPE_PIPE || GetFileType(output)!=FILE_TYPE_PIPE)return 2;
  const HANDLE readable = argc >= 4 ? reinterpret_cast<HANDLE>(
      static_cast<std::uintptr_t>(std::strtoull(argv[3], nullptr, 10))) : nullptr;
  const HANDLE final_ready=argc==5?reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(argv[4],nullptr,10))):nullptr;
  const auto reply = [&](std::uint32_t op, const std::vector<std::string>& fields) {
    return model_worker::Packet(output, op, model_worker::Fields(fields), readable);
  };
  std::unique_ptr<Recognizer> recognizer;size_t samples=0;
  bool draft_changes_only=false;
#ifdef VOICEIME_WORKER_TESTING
  char fixture_buffer[64]{};GetEnvironmentVariableA("VOICEIME_MODEL_WORKER_FIXTURE",fixture_buffer,sizeof(fixture_buffer));
  const std::string fixture=fixture_buffer;bool initialized=false;int audio_requests=0;
#endif
  for(;;) {
    model_worker::Header header{};
    if(!model_worker::Read(input,&header,sizeof(header)))return 0;
    if(header.magic!=model_worker::kMagic || header.size>model_worker::kLimit)return 3;
    std::string bytes(header.size,'\0');if(!model_worker::Read(input,bytes.data(),bytes.size()))return 3;
    try {
      const bool initializing=header.operation==model_worker::Init || header.operation==model_worker::InitDraft;
      // Apply the same wire bounds to production recognizers and fault fixtures.
      if(header.operation==model_worker::Audio) {
        if(bytes.size()%2 || !FitsRecording(samples,bytes.size()/2))throw std::runtime_error("音频长度无效。");
        samples+=bytes.size()/2;
      } else if(header.operation==model_worker::Reset) {
        if(!bytes.empty())throw std::runtime_error("无效的重置数据。");
        samples=0;
      } else if(header.operation==model_worker::Finalize && !bytes.empty())throw std::runtime_error("无效的定稿数据。");
#ifdef VOICEIME_WORKER_TESTING
      if(!fixture.empty()) {
        if(initializing) {
          if(initialized)throw std::runtime_error("重复初始化");
          if(fixture=="init-crash")ExitProcess(17);
          if(fixture=="init-error")throw std::runtime_error("测试：模型文件不匹配");
          if(fixture=="init-hang")Sleep(INFINITE);
          initialized=true;
          if(final_ready && (fixture!="staged" || header.operation==model_worker::Init)) SetEvent(final_ready);
        } else if(!initialized)throw std::runtime_error("尚未初始化");
        if(final_ready && fixture=="staged" && header.operation==model_worker::Finalize) SetEvent(final_ready);
        if(header.operation==model_worker::Finalize && fixture=="final-hang")Sleep(INFINITE);
        if(header.operation==model_worker::Audio && fixture=="audio-crash")ExitProcess(18);
        if(header.operation==model_worker::Audio && fixture=="audio-hang" && ++audio_requests>1)Sleep(INFINITE);
        std::string result=header.operation==model_worker::Audio?"测试草稿":
          header.operation==model_worker::Finalize?"测试最终文本":"";
        if(fixture=="staged" && header.operation==model_worker::Reset)audio_requests=0;
        if(fixture=="staged" && header.operation==model_worker::Audio && ++audio_requests>1)result.clear();
        if (fixture=="large-final" && header.operation==model_worker::Finalize)
          result.assign(128 * 1024, 'x');
        if(!reply(header.operation,{result,result,""}))return 0;
        continue;
      }
#endif
      if(initializing) {
        std::vector<std::string> fields;ModelSettings settings;
        if(recognizer || !model_worker::Fields(bytes,2,fields) || !ParseModelSettings(fields[1],settings))
          throw std::runtime_error("模型设置无效。");
        draft_changes_only=header.operation==model_worker::InitDraft;
        const auto startup=draft_changes_only?ModelStartup::DraftFirst:ModelStartup::Full;
        std::string error;
        recognizer=MakeSherpaRecognizer(fields[0],settings,&error,startup,[final_ready,startup](bool ok) {
          if(ok && final_ready) SetEvent(final_ready);
          // Strict activation returns its normal error reply. During staged
          // startup the host already has a draft; a failed worker must become
          // visibly unhealthy even if the user has not started speaking yet.
          if(!ok && startup==ModelStartup::DraftFirst) ExitProcess(4);
        });
        if(!recognizer)throw std::runtime_error(error.empty()?"所选模型无法加载，请重新下载或选择完整文件夹。":error);
      } else if(!recognizer)throw std::runtime_error("模型尚未准备。");
      else if(header.operation==model_worker::Audio) {
        std::vector<std::int16_t> wave(bytes.size()/2);
        std::memcpy(wave.data(),bytes.data(),bytes.size());
        const bool changed=recognizer->AcceptAudio(wave.data(),wave.size());
        if(draft_changes_only && !changed) {
          if(!reply(header.operation,{"","",""}))return 0;
          continue;
        }
      } else if(header.operation==model_worker::Reset) {
        recognizer->Reset();
      } else if(header.operation==model_worker::Finalize) {
        const auto text=recognizer->Finalize();
        if(!reply(header.operation,{text,recognizer->Original(),recognizer->TakeWarning()}))return 0;
        continue;
      } else throw std::runtime_error("无效的模型操作。");
      if(!reply(header.operation,{recognizer?recognizer->Partial():"","",""}))return 0;
    } catch(const std::exception& e) {
      reply(model_worker::Error,{e.what()});return 4;
    } catch(...) {
      reply(model_worker::Error,{"模型操作失败，请重新准备模型。"});return 4;
    }
  }
}
