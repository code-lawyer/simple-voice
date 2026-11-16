#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace model_worker {
constexpr std::uint32_t kMagic=0x31564d53, kLimit=2*1024*1024;
enum Operation : std::uint32_t { Init=1, Audio=2, Reset=3, Finalize=4, Error=0xffffffffu };
struct Header { std::uint32_t magic=kMagic, operation=0, size=0; };
inline bool Write(HANDLE pipe,const void* bytes,size_t size) {
  const auto* p=static_cast<const char*>(bytes);
  while(size) { DWORD sent=0; if(!WriteFile(pipe,p,static_cast<DWORD>((std::min)(size,size_t(65536))),&sent,nullptr)||!sent)return false;
    p+=sent; size-=sent; }
  return true;
}
inline bool Read(HANDLE pipe,void* bytes,size_t size) {
  auto* p=static_cast<char*>(bytes);
  while(size) { DWORD got=0; if(!ReadFile(pipe,p,static_cast<DWORD>(size),&got,nullptr)||!got)return false; p+=got;size-=got; }
  return true;
}
inline bool Packet(HANDLE pipe,std::uint32_t operation,const std::string& bytes) {
  if(bytes.size()>kLimit)return false;
  Header header{kMagic,operation,static_cast<std::uint32_t>(bytes.size())};
  return Write(pipe,&header,sizeof(header)) && Write(pipe,bytes.data(),bytes.size());
}
inline std::string Fields(const std::vector<std::string>& fields) {
  std::string bytes;
  for(const auto& field:fields) {
    const std::uint32_t n=static_cast<std::uint32_t>(field.size());
    bytes.append(reinterpret_cast<const char*>(&n),sizeof(n));bytes+=field;
  }
  return bytes;
}
inline bool Fields(const std::string& bytes,size_t count,std::vector<std::string>& fields) {
  fields.clear();size_t at=0;
  while(at<bytes.size() && fields.size()<count) {
    if(bytes.size()-at<4)return false;
    std::uint32_t n=0;std::memcpy(&n,bytes.data()+at,4);at+=4;
    if(n>bytes.size()-at)return false;
    fields.push_back(bytes.substr(at,n));at+=n;
  }
  return at==bytes.size() && fields.size()==count;
}
}
