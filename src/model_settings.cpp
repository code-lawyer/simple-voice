#include "model_settings.hpp"
#include "utf8.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <charconv>
#include <cstdio>
#include <io.h>

namespace {
bool Text(const std::string& value, size_t limit) {
  return value.size()<=limit && value.find('\0')==std::string::npos &&
    value.find_first_of("\r\n")==std::string::npos &&
    (value.empty() || MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
                                       static_cast<int>(value.size()),nullptr,0)>0);
}
bool Valid(const ModelSettings& s) {
  if(!SpeechModelId(s.model) || s.idle_minutes<0 || s.idle_minutes>60 || !Text(s.hotwords,2048)) return false;
  for(const auto& path:s.directories) if(!Text(WideToUtf8(path),2048)) return false;
  return true;
}
std::string Join(const std::string& root,const std::string& part) {
  return WideToUtf8((std::filesystem::path(Utf8ToWide(root))/Utf8ToWide(part)).wstring());
}
bool File(const std::string& path) {
  std::error_code error;
  const auto p=std::filesystem::path(Utf8ToWide(path));
  return std::filesystem::is_regular_file(p,error) && std::filesystem::file_size(p,error)>0 && !error;
}
}
const char* SpeechModelId(SpeechModel m) {
  switch(m) { case SpeechModel::Qwen3:return "qwen3"; case SpeechModel::SenseVoice:return "sensevoice";
    case SpeechModel::Whisper:return "whisper"; case SpeechModel::ChineseLegacy:return "chinese-legacy"; }
  return nullptr;
}
const wchar_t* SpeechModelLabel(SpeechModel m) {
  switch(m) { case SpeechModel::Qwen3:return L"Qwen3-ASR · 推荐";
    case SpeechModel::SenseVoice:return L"SenseVoiceSmall · 轻量";
    case SpeechModel::Whisper:return L"Whisper Turbo · 多语言";
    case SpeechModel::ChineseLegacy:return L"旧中文模型 · 兼容"; }
  return L"未知模型";
}
const wchar_t* SpeechModelDescription(SpeechModel m) {
  switch(m) {
    case SpeechModel::Qwen3:return L"适合中英混说和英文术语。CPU 定稿较慢，占用内存较多。";
    case SpeechModel::SenseVoice:return L"适合日常普通话短句，定稿较快、内存占用较低。也支持英语、粤语、日语和韩语。";
    case SpeechModel::Whisper:return L"多语言备选。不同语言的速度和准确度取决于本机表现。";
    case SpeechModel::ChineseLegacy:return L"保留旧版中文优先方案，不适合英语或中英混说。";
  }
  return L"";
}
const char* SpeechModelFolder(SpeechModel m) {
  switch(m) { case SpeechModel::Qwen3:return "final-qwen"; case SpeechModel::SenseVoice:return "final-sense";
    case SpeechModel::Whisper:return "final-whisper"; case SpeechModel::ChineseLegacy:return "final"; }
  return "";
}
std::string SerializeModelSettings(const ModelSettings& s) {
  if(!Valid(s)) return {};
  std::string result="version=1\nmodel="+std::string(SpeechModelId(s.model))+
      "\nidle="+std::to_string(s.idle_minutes)+"\nhotwords="+s.hotwords+"\n";
  for(int i=0;i<4;++i) result+="directory"+std::to_string(i)+"="+WideToUtf8(s.directories[i])+"\n";
  return result;
}
bool ParseModelSettings(const std::string& text,ModelSettings& result) {
  if(text.empty() || text.size()>12288 || text.find('\0')!=std::string::npos) return false;
  std::map<std::string,std::string> fields; std::istringstream input(text); std::string line;
  while(std::getline(input,line)) {
    if(!line.empty() && line.back()=='\r') line.pop_back();
    const auto equal=line.find('=');
    if(equal==std::string::npos || !fields.emplace(line.substr(0,equal),line.substr(equal+1)).second) return false;
  }
  if(fields.size()!=8 || !fields.count("model") || !fields.count("idle") || !fields.count("hotwords") ||
     fields["version"]!="1") return false;
  ModelSettings next; bool known=false;
  for(auto model:{SpeechModel::Qwen3,SpeechModel::SenseVoice,SpeechModel::Whisper,SpeechModel::ChineseLegacy})
    if(fields["model"]==SpeechModelId(model)) { next.model=model; known=true; }
  if(!known) return false;
  const auto& idle=fields["idle"];
  const auto number=std::from_chars(idle.data(),idle.data()+idle.size(),next.idle_minutes);
  if(number.ec!=std::errc{} || number.ptr!=idle.data()+idle.size()) return false;
  next.hotwords=fields["hotwords"];
  for(int i=0;i<4;++i) {
    const auto found=fields.find("directory"+std::to_string(i));
    if(found==fields.end() || !Text(found->second,2048)) return false;
    next.directories[i]=Utf8ToWide(found->second);
  }
  if(!Valid(next)) return false;
  result=std::move(next); return true;
}
ModelSettings LoadModelSettings(const std::wstring& path,bool legacy_chinese) {
  ModelSettings result; if(legacy_chinese) result.model=SpeechModel::ChineseLegacy;
  std::ifstream file(std::filesystem::path(path),std::ios::binary);
  if(!file) return result;
  std::string data(12289,'\0'); file.read(data.data(),data.size()); data.resize(static_cast<size_t>(file.gcount()));
  ParseModelSettings(data,result); return result;
}
bool SaveModelSettings(const std::wstring& path,const ModelSettings& s) {
  const auto data=SerializeModelSettings(s); if(data.empty()) return false;
  const auto temp=path+L".tmp";
  FILE* f=_wfopen(temp.c_str(),L"wb"); if(!f) return false;
  bool ok=std::fwrite(data.data(),1,data.size(),f)==data.size();
  if(std::fflush(f)!=0 || _commit(_fileno(f))!=0) ok=false;
  if(std::fclose(f)!=0) ok=false;
  if(ok) ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
  if(!ok) DeleteFileW(temp.c_str()); return ok;
}
bool SameModelRuntime(const ModelSettings& a,const ModelSettings& b) {
  if(!SpeechModelId(a.model) || !SpeechModelId(b.model))return false;
  return a.model==b.model && a.directories[static_cast<int>(a.model)]==b.directories[static_cast<int>(b.model)] &&
      (a.model!=SpeechModel::Qwen3 || a.hotwords==b.hotwords);
}
bool ResolveModelFiles(const std::string& root,const ModelSettings& settings,ModelFiles& f,std::string& error) {
  if(!Valid(settings)) { error="模型设置无效，请恢复推荐方案。"; return false; }
  f={}; f.model=settings.model; f.hotwords=settings.model==SpeechModel::Qwen3?settings.hotwords:"";
  const auto custom=WideToUtf8(settings.directories[static_cast<int>(settings.model)]);
  const auto final_dir=custom.empty()?Join(root,SpeechModelFolder(settings.model)):custom;
  auto required=[&](const std::string& dir,const std::string& name,std::string& out) {
    out=Join(dir,name);
    if(File(out)) return true;
    error="缺少或无法读取 "+name+"。请下载此模型，或选择完整的兼容模型文件夹。"; return false;
  };
  const auto draft=Join(root,"streaming-bilingual");
  if(!required(draft,"encoder-epoch-99-avg-1.int8.onnx",f.stream_encoder) ||
     !required(draft,"decoder-epoch-99-avg-1.onnx",f.stream_decoder) ||
     !required(draft,"joiner-epoch-99-avg-1.int8.onnx",f.stream_joiner) ||
     !required(draft,"tokens.txt",f.stream_tokens)) return false;
  if(settings.model==SpeechModel::Qwen3) {
    if(!required(final_dir,"conv_frontend.onnx",f.final_model) ||
       !required(final_dir,"encoder.int8.onnx",f.final_encoder) ||
       !required(final_dir,"decoder.int8.onnx",f.final_decoder)) return false;
    f.final_tokens=Join(final_dir,"tokenizer");
    std::string unused;
    for(const char* name:{"vocab.json","merges.txt","tokenizer_config.json"})
      if(!required(f.final_tokens,name,unused)) return false;
  } else if(settings.model==SpeechModel::Whisper) {
    if(!required(final_dir,"turbo-encoder.int8.onnx",f.final_encoder) ||
       !required(final_dir,"turbo-decoder.int8.onnx",f.final_decoder) ||
       !required(final_dir,"turbo-tokens.txt",f.final_tokens)) return false;
  } else {
    if(!required(final_dir,"model.int8.onnx",f.final_model) ||
       !required(final_dir,"tokens.txt",f.final_tokens)) return false;
    if(settings.model==SpeechModel::ChineseLegacy &&
       !required(Join(root,"punct"),"model.int8.onnx",f.punctuation)) return false;
  }
  error.clear(); return true;
}
