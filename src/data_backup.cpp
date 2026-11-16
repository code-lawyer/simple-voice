#include "data_backup.hpp"
#include "utf8.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <cstring>
#include <charconv>
#include <cstdio>
#include <io.h>
#include <stdexcept>

namespace fs=std::filesystem;
namespace {
constexpr size_t kLimit=12*1024*1024;
const std::array<const wchar_t*,3> names={L"preferences.ini",L"config.json",L"appearance.json"};
struct Snapshot {std::array<std::string,3> files;std::array<bool,3> exists{};std::vector<Entry> terms;};
void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void Number(std::string& out,uint32_t value){for(int i=0;i<4;++i)out.push_back(static_cast<char>(value>>(i*8)));}
uint32_t Number(std::string_view& in){Require(in.size()>=4,"备份文件不完整");uint32_t value=0;for(int i=0;i<4;++i)value|=uint32_t(static_cast<unsigned char>(in[i]))<<(i*8);in.remove_prefix(4);return value;}
void Field(std::string& out,const std::string& value){Number(out,static_cast<uint32_t>(value.size()));out+=value;}
std::string Field(std::string_view& in,size_t limit){const auto n=Number(in);Require(n<=limit && n<=in.size(),"备份字段长度无效");std::string result(in.substr(0,n));in.remove_prefix(n);return result;}
std::string Hash(const std::string& value) {
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
  Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"无法校验备份");
  DWORD bytes=0,size=0;bool ok=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&bytes,0)>=0;
  std::vector<unsigned char> object(size);std::string digest(32,'\0');
  if(ok)ok=BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)>=0;
  if(ok)ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),static_cast<ULONG>(value.size()),0)>=0;
  if(ok)ok=BCryptFinishHash(hash,reinterpret_cast<PUCHAR>(digest.data()),32,0)>=0;
  if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);Require(ok,"无法校验备份");return digest;
}
std::string Read(const fs::path& path,size_t limit) {
  const auto attr=GetFileAttributesW(path.c_str());
  Require(attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_REPARSE_POINT) && !(attr&FILE_ATTRIBUTE_DIRECTORY),"备份文件不存在或不是普通文件");
  std::ifstream file(path,std::ios::binary|std::ios::ate);Require(bool(file),"无法读取文件");
  const auto size=file.tellg();Require(size>=0 && static_cast<uint64_t>(size)<=limit,"备份文件过大");
  std::string out(static_cast<size_t>(size),'\0');file.seekg(0);file.read(out.data(),out.size());Require(bool(file),"文件读取不完整");return out;
}
void Write(const fs::path& path,const std::string& bytes) {
  if(fs::exists(path) && Read(path,kLimit)==bytes)return;
  const auto temp=path.wstring()+L".restore-tmp";
  FILE* file=_wfopen(temp.c_str(),L"wb");Require(file!=nullptr,"无法写入数据，请检查文件权限");
  bool ok=std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
  if(std::fflush(file)!=0 || _commit(_fileno(file))!=0)ok=false;
  if(std::fclose(file)!=0)ok=false;
  if(ok)ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
  if(!ok)DeleteFileW(temp.c_str());Require(ok,"数据正在使用或无法保存，请稍后重试");
}
Snapshot Current(Lexicon& lexicon,const fs::path& data) {
  Snapshot s;s.terms=lexicon.Search("",10000);
  for(size_t i=0;i<names.size();++i){s.exists[i]=fs::exists(data/names[i]);if(s.exists[i])s.files[i]=Read(data/names[i],16384);}return s;
}
std::string Encode(const Snapshot& s) {
  std::string out="SimpleVoiceBackup\n1\n";
  for(size_t i=0;i<names.size();++i){Number(out,s.exists[i]?1:0);Field(out,s.files[i]);}
  Number(out,static_cast<uint32_t>(s.terms.size()));
  for(const auto& row:s.terms){Field(out,row.text);Field(out,row.pinyin);Number(out,row.count);Field(out,std::to_string(row.last_commit_unix));}
  Require(out.size()<=kLimit-32,"备份内容过大");return out+Hash(out);
}
Snapshot Decode(const std::string& bytes) {
  const std::string magic="SimpleVoiceBackup\n1\n";
  Require(bytes.size()>magic.size()+32 && bytes.compare(0,magic.size(),magic)==0,"不是支持的轻音备份格式");
  const auto body=bytes.substr(0,bytes.size()-32);Require(Hash(body)==bytes.substr(bytes.size()-32),"备份校验失败，文件可能损坏；未修改当前数据");
  std::string_view in(body);in.remove_prefix(magic.size());Snapshot s;
  for(size_t i=0;i<names.size();++i){const auto exists=Number(in);Require(exists<=1,"备份字段无效");s.exists[i]=exists!=0;s.files[i]=Field(in,16384);Require(exists || s.files[i].empty(),"备份字段无效");}
  const auto count=Number(in);Require(count<=10000,"备份词语过多");
  for(uint32_t i=0;i<count;++i){Entry row;row.text=Field(in,512);row.pinyin=Field(in,256);row.count=static_cast<int>(Number(in));auto time=Field(in,24);
    const auto parsed=std::from_chars(time.data(),time.data()+time.size(),row.last_commit_unix);Require(parsed.ec==std::errc{} && parsed.ptr==time.data()+time.size(),"备份日期无效");row.kind="user";s.terms.push_back(std::move(row));}
  Require(in.empty(),"备份存在未识别字段");return s;
}
void Files(const Snapshot& s,const fs::path& data) {
  for(size_t i=0;i<names.size();++i) {
    const auto target=data/names[i];
    if(s.exists[i])Write(target,s.files[i]);
    else if(fs::exists(target))Require(DeleteFileW(target.c_str())!=FALSE,"无法恢复原设置文件");
  }
}
fs::path Journal(const fs::path& data){return data/L"restore-recovery.backup";}
}
std::wstring CreateDataBackup(Lexicon& lexicon,const std::wstring& data,const std::wstring& parent) {
  const auto bytes=Encode(Current(lexicon,fs::path(data)));
  SYSTEMTIME now{};GetLocalTime(&now);wchar_t stamp[96];
  swprintf(stamp,96,L"SimpleVoice-%04u%02u%02u-%02u%02u%02u-%llu",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetTickCount64());
  const auto folder=fs::path(parent)/stamp;Require(fs::create_directories(folder),"无法创建备份文件夹");
  Write(folder/L"SimpleVoice.backup",bytes);return folder.wstring();
}
bool RecoverDataRestore(Lexicon& lexicon,const std::wstring& data) {
  const auto journal=Journal(data);if(!fs::exists(journal))return false;
  const auto old=Decode(Read(journal,kLimit));
  lexicon.ReplaceTerms(old.terms,[&]{Files(old,data);});
  Require(DeleteFileW(journal.c_str())!=FALSE,"恢复已完成，但无法清理恢复记录");return true;
}
void RestoreDataBackup(Lexicon& lexicon,const std::wstring& data,const std::wstring& backup) {
  const auto next=Decode(Read(fs::path(backup)/L"SimpleVoice.backup",kLimit));
  // Validation in a disposable in-memory database leaves the live database untouched.
  {Lexicon validation(":memory:");validation.ReplaceTerms(next.terms);}
  Require(!fs::exists(Journal(data)),"上次恢复尚未完成，请重启轻音后再试");
  CreateDataBackup(lexicon,data,(fs::path(data)/L"backups").wstring());
  const auto old=Current(lexicon,data);Write(Journal(data),Encode(old));
  try {
    lexicon.ReplaceTerms(next.terms,[&]{Files(next,data);});
    Require(DeleteFileW(Journal(data).c_str())!=FALSE,"无法完成恢复记录");
  } catch(...) {
    try {lexicon.ReplaceTerms(old.terms,[&]{Files(old,data);});DeleteFileW(Journal(data).c_str());}
    catch(...) {throw std::runtime_error("恢复未完成。旧数据已备份，请关闭轻音并重启以恢复原数据。");}
    throw;
  }
}
