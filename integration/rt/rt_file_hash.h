#pragma once
#include <windows.h>
#include <bcrypt.h>
#include "rt_runtime_profile.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace ldb_rt_file {
namespace fs = std::filesystem;
// The provider is immutable and shared; each invocation still owns an independent
// hash/object. This removes per-Draw provider opens without caching mutable input.
struct Sha256Provider {
  BCRYPT_ALG_HANDLE algorithm=nullptr;DWORD objectBytes=0;
  Sha256Provider() {
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) { algorithm=nullptr;return; }
    DWORD got=0;if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&got,0)<0||got!=sizeof(objectBytes)||!objectBytes) {
      BCryptCloseAlgorithmProvider(algorithm,0);algorithm=nullptr;
    }
  }
  ~Sha256Provider() { if(algorithm) { BCryptCloseAlgorithmProvider(algorithm,0); } }
  Sha256Provider(const Sha256Provider&)=delete;Sha256Provider& operator=(const Sha256Provider&)=delete;
};
inline const Sha256Provider& sha256Provider() { static const Sha256Provider value;return value; }
// Geometry identity is evaluated several times for every persistent skinned
// Draw.  A reusable BCrypt handle keeps the exact SHA-256 identity while
// avoiding a heap allocation and CreateHash/DestroyHash pair for every input.
// The handle is thread-local: BCrypt hash handles are mutable and must never be
// shared by future parallel preparation workers.
struct ReusableSha256 {
  BCRYPT_HASH_HANDLE hash=nullptr;std::vector<unsigned char> object;
  ReusableSha256() {
    const auto& provider=sha256Provider();if(!provider.algorithm) { return; }
    object.resize(provider.objectBytes);
    if(BCryptCreateHash(provider.algorithm,&hash,object.data(),provider.objectBytes,nullptr,0,BCRYPT_HASH_REUSABLE_FLAG)<0) { hash=nullptr;object.clear(); }
  }
  ~ReusableSha256() { if(hash) { BCryptDestroyHash(hash); } }
  ReusableSha256(const ReusableSha256&)=delete;ReusableSha256& operator=(const ReusableSha256&)=delete;
};
inline ReusableSha256& reusableSha256() { thread_local ReusableSha256 value;return value; }
inline std::string bytesSha256(const void* data,size_t size,size_t limit=65536) {
  if (!data || !size || size>limit) { return {}; }
  ldb_rt::runtime_profile::Scope timing(ldb_rt::runtime_profile::Stage::Hash);
  unsigned char bytes[32]{};auto& reusable=reusableSha256();bool complete=false;
  if(reusable.hash) {
    complete=BCryptHashData(reusable.hash,static_cast<PUCHAR>(const_cast<void*>(data)),ULONG(size),0)>=0&&
      BCryptFinishHash(reusable.hash,bytes,sizeof(bytes),0)>=0;
  }
  // Windows 11 supports reusable hashes. Keep the original independent-handle
  // path as a correctness fallback for an older CNG provider or a failed
  // reusable operation.
  if(!complete) {
    const auto& provider=sha256Provider();if(!provider.algorithm) { return {}; }BCRYPT_HASH_HANDLE hash=nullptr;
    struct Cleanup { BCRYPT_HASH_HANDLE& h;~Cleanup() { if(h) { BCryptDestroyHash(h); } } } cleanup{hash};
    std::vector<unsigned char> object(provider.objectBytes);
    if(BCryptCreateHash(provider.algorithm,&hash,object.data(),provider.objectBytes,nullptr,0,0)<0||
       BCryptHashData(hash,static_cast<PUCHAR>(const_cast<void*>(data)),ULONG(size),0)<0||
       BCryptFinishHash(hash,bytes,sizeof(bytes),0)<0) { return {}; }
  }
  static const char hex[]="0123456789abcdef";std::string result;
  for (auto c:bytes) { result+=hex[c>>4];result+=hex[c&15]; }return result;
}
// Geometry uses the audited 32K-vertex/128K-index budgets, unlike shader bytecode.
inline std::string geometrySha256(const void* data,size_t size) {
  return size?bytesSha256(data,size,4u*1024*1024):"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
}
inline std::string fileSha256(const fs::path& path) {
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  struct Cleanup {
    BCRYPT_ALG_HANDLE& a; BCRYPT_HASH_HANDLE& h;
    ~Cleanup() { if (h) { BCryptDestroyHash(h); } if (a) { BCryptCloseAlgorithmProvider(a, 0); } }
  } cleanup{algorithm, hash};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) { return {}; }
  DWORD objectSize = 0, got = 0;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &got, 0) < 0) { return {}; }
  std::vector<unsigned char> object(objectSize);
  if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) { return {}; }
  std::ifstream input(path, std::ios::binary); if (!input) { return {}; }
  unsigned char block[65536];
  while (input) {
    input.read(reinterpret_cast<char*>(block), sizeof(block));
    if (input.gcount() && BCryptHashData(hash, block, ULONG(input.gcount()), 0) < 0) { return {}; }
  }
  if (!input.eof()) { return {}; }
  unsigned char bytes[32]{};
  if (BCryptFinishHash(hash, bytes, sizeof(bytes), 0) < 0) { return {}; }
  static const char hex[] = "0123456789abcdef"; std::string result;
  for (auto c : bytes) { result += hex[c >> 4]; result += hex[c & 15]; } return result;
}

}
