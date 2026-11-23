#include "sha256.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <stdexcept>
#include <vector>

std::string Sha256(const std::string& value) {
  BCRYPT_ALG_HANDLE algorithm=nullptr;
  BCRYPT_HASH_HANDLE hash=nullptr;
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)
    throw std::runtime_error("无法打开 SHA-256 校验");
  DWORD bytes=0,size=0;
  bool ok=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&bytes,0)>=0;
  std::vector<unsigned char> object(size);
  std::string digest(32,'\0');
  if(ok) ok=BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)>=0;
  if(ok) ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),static_cast<ULONG>(value.size()),0)>=0;
  if(ok) ok=BCryptFinishHash(hash,reinterpret_cast<PUCHAR>(digest.data()),32,0)>=0;
  if(hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm,0);
  if(!ok) throw std::runtime_error("SHA-256 校验失败");
  return digest;
}
