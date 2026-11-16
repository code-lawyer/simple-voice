#include "worker_protocol.hpp"
#include "sherpa_recognizer.hpp"
#include "model_settings.hpp"
#include <cstdlib>
#include <memory>
#include <stdexcept>

int main(int argc,char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  if(argc!=3)return 2;
  const auto input=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(argv[1],nullptr,10)));
  const auto output=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(argv[2],nullptr,10)));
  if(GetFileType(input)!=FILE_TYPE_PIPE || GetFileType(output)!=FILE_TYPE_PIPE)return 2;
  std::unique_ptr<Recognizer> recognizer;size_t samples=0;
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
#ifdef VOICEIME_WORKER_TESTING
      if(!fixture.empty()) {
        if(header.operation==model_worker::Init) {
          if(initialized)throw std::runtime_error("重复初始化");
          if(fixture=="init-crash")ExitProcess(17);
          if(fixture=="init-error")throw std::runtime_error("测试：模型文件不匹配");
          if(fixture=="init-hang")Sleep(INFINITE);
          initialized=true;
        } else if(!initialized)throw std::runtime_error("尚未初始化");
        if(header.operation==model_worker::Finalize && fixture=="final-hang")Sleep(INFINITE);
        if(header.operation==model_worker::Audio && fixture=="audio-crash")ExitProcess(18);
        if(header.operation==model_worker::Audio && fixture=="audio-hang" && ++audio_requests>1)Sleep(INFINITE);
        const std::string result=header.operation==model_worker::Audio?"测试草稿":
          header.operation==model_worker::Finalize?"测试最终文本":"";
        if(!model_worker::Packet(output,header.operation,model_worker::Fields({result,result,""})))return 0;
        continue;
      }
#endif
      if(header.operation==model_worker::Init) {
        std::vector<std::string> fields;ModelSettings settings;
        if(recognizer || !model_worker::Fields(bytes,2,fields) || !ParseModelSettings(fields[1],settings))
          throw std::runtime_error("模型设置无效。");
        std::string error;recognizer=MakeSherpaRecognizer(fields[0],settings,&error);
        if(!recognizer)throw std::runtime_error(error.empty()?"所选模型无法加载，请重新下载或选择完整文件夹。":error);
      } else if(!recognizer)throw std::runtime_error("模型尚未准备。");
      else if(header.operation==model_worker::Audio) {
        if(bytes.size()%2 || samples+bytes.size()/2>16000*61)throw std::runtime_error("音频长度无效。");
        std::vector<std::int16_t> wave(bytes.size()/2);
        std::memcpy(wave.data(),bytes.data(),bytes.size());samples+=wave.size();
        recognizer->AcceptAudio(wave.data(),wave.size());
      } else if(header.operation==model_worker::Reset) {
        if(!bytes.empty())throw std::runtime_error("无效的重置数据。");
        recognizer->Reset();samples=0;
      } else if(header.operation==model_worker::Finalize) {
        if(!bytes.empty())throw std::runtime_error("无效的定稿数据。");
        const auto text=recognizer->Finalize();
        if(!model_worker::Packet(output,header.operation,
             model_worker::Fields({text,recognizer->Original(),recognizer->TakeWarning()})))return 0;
        continue;
      } else throw std::runtime_error("无效的模型操作。");
      if(!model_worker::Packet(output,header.operation,
          model_worker::Fields({recognizer?recognizer->Partial():"","",""})))return 0;
    } catch(const std::exception& e) {
      model_worker::Packet(output,model_worker::Error,model_worker::Fields({e.what()}));return 4;
    } catch(...) {
      model_worker::Packet(output,model_worker::Error,model_worker::Fields({"模型操作失败，请重新准备模型。"}));return 4;
    }
  }
}
