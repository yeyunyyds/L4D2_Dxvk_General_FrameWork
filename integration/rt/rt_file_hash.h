#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace ldb_rt_file {
namespace fs = std::filesystem;
inline std::string bytesSha256(const void* data,size_t size,size_t limit=65536) {
  if (!data || !size || size>limit) { return {}; }
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
  struct Cleanup { BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Cleanup() { if (h) { BCryptDestroyHash(h); }if (a) { BCryptCloseAlgorithmProvider(a,0); } } } cleanup{algorithm,hash};
  if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) { return {}; }
  DWORD sizeObject=0,got=0;
  if (BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&sizeObject),sizeof(sizeObject),&got,0)<0) { return {}; }
  std::vector<unsigned char> object(sizeObject);
  if (BCryptCreateHash(algorithm,&hash,object.data(),sizeObject,nullptr,0,0)<0 ||
      BCryptHashData(hash,static_cast<PUCHAR>(const_cast<void*>(data)),ULONG(size),0)<0) { return {}; }
  unsigned char bytes[32]{};if (BCryptFinishHash(hash,bytes,sizeof(bytes),0)<0) { return {}; }
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
