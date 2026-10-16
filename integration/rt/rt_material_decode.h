#pragma once
#include "rt_protocol.h"

// Bounded data decoders for the exact leaf code observed in the game.
// No private function is called, and unsupported code is never guessed.
namespace ldb_rt::material_decode {
enum : uint32_t { FormatRead = 1, ShaderRead = 2 };
struct Info {
  uint64_t format = 0;
  uint32_t flags = 0, shaderObject = 0, shaderMethod = 0;
  uint32_t nameSlot = 0, nameAddress = 0; // Diagnostic only; not added to the wire.
  std::array<char, 64> shader{};
};
template<class Memory> bool read(Memory& memory, uint32_t base, uint32_t offset, void* out, size_t size) {
  return base && uint64_t(base) + offset + size <= UINT32_MAX && memory.read(base + offset, out, size);
}
template<class Memory> bool text(Memory& memory, uint32_t address, std::array<char, 64>& out) {
  out.fill(0);
  for (uint32_t i = 0; i < out.size(); ++i) {
    char c = 0;
    if (!read(memory, address, i, &c, 1)) { out.fill(0); return false; }
    if (!c) { return i != 0; }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
      out.fill(0); return false;
    }
    out[i] = c;
  }
  out.fill(0); return false; // Never silently truncate a classification string.
}
template<class Memory> Info decode(Memory& memory, uint32_t object, uint32_t table) {
  Info result;
  uint32_t actualTable = 0, formatMethod = 0, shaderGetter = 0;
  if (!read(memory, object, 0, &actualTable, 4) || actualTable != table ||
      !read(memory, table, 20 * 4, &formatMethod, 4)) { return result; }
  const uint8_t formatBody[] = {0x8b,0x41,0x38,0x8b,0x51,0x3c,0xc3};
  uint8_t actualFormat[sizeof(formatBody)]{};
  if (!memory.code(formatMethod, actualFormat, sizeof(actualFormat)) ||
      std::memcmp(actualFormat, formatBody, sizeof(formatBody)) ||
      !read(memory, object, 0x38, &result.format, 8)) { return result; }
  result.flags |= FormatRead;
  if (!read(memory, table, 49 * 4, &shaderGetter, 4)) { return result; }
  const uint8_t getterPrefix[] = {0x83,0x79,0x14,0x00,0x74,0x09,0x8b,0x49,0x14,0x8b,0x01,0x8b,0x10,0xff,0xe2};
  uint8_t getter[sizeof(getterPrefix)]{};
  if (!memory.code(shaderGetter, getter, sizeof(getter)) || std::memcmp(getter, getterPrefix, sizeof(getter)) ||
      !read(memory, object, 0x14, &result.shaderObject, 4) || !result.shaderObject) { return result; }
  uint32_t shaderTable = 0;
  if (!read(memory, result.shaderObject, 0, &shaderTable, 4) ||
      !read(memory, shaderTable, 0, &result.shaderMethod, 4)) { return result; }
  uint8_t body[8]{};
  if (!memory.code(result.shaderMethod, body, sizeof(body))) { return result; }
  uint32_t string = 0;
  if (body[0] == 0xb8 && body[5] == 0xc3) {
    std::memcpy(&string, body + 1, 4); // mov eax, <literal>; ret
  } else if (body[0] == 0xa1 && body[5] == 0xc3) {
    // Actual stdshader_dx9 GetName leaf: mov eax, [absolute pointer]; ret.
    // Read the pointer cell as data; never execute the getter.
    std::memcpy(&result.nameSlot, body + 1, 4);
    if (!read(memory, result.nameSlot, 0, &string, 4)) { return result; }
  } else if (body[0] == 0x8b && body[1] == 0x41 && body[3] == 0xc3 && body[2] < 0x80) {
    if (!read(memory, result.shaderObject, body[2], &string, 4)) { return result; }
    result.nameSlot = result.shaderObject + body[2];
  } else { return result; }
  result.nameAddress = string;
  if (!text(memory, string, result.shader)) { return result; }
  std::array<char,64> repeatedName{};
  uint8_t repeatedCode[sizeof(body)]{};
  uint32_t repeatedTable=0,repeatedMethod=0,repeatedString=string;
  if (!text(memory,string,repeatedName) || repeatedName!=result.shader ||
      !memory.code(result.shaderMethod,repeatedCode,sizeof(repeatedCode)) || std::memcmp(body,repeatedCode,sizeof(body)) ||
      !read(memory,result.shaderObject,0,&repeatedTable,4) || repeatedTable!=shaderTable ||
      !read(memory,shaderTable,0,&repeatedMethod,4) || repeatedMethod!=result.shaderMethod ||
      (result.nameSlot && (!read(memory,result.nameSlot,0,&repeatedString,4) || repeatedString!=string))) { return Info{}; }
  uint32_t afterObject = 0, afterTable = 0;
  uint64_t afterFormat = 0;
  if (!read(memory, object, 0x14, &afterObject, 4) || afterObject != result.shaderObject ||
      !read(memory, object, 0, &afterTable, 4) || afterTable != table ||
      !read(memory, object, 0x38, &afterFormat, 8) || afterFormat != result.format) { return Info{}; }
  result.flags |= ShaderRead; return result;
}
inline bool worldShader(const std::array<char, 64>& value) noexcept {
  return std::strncmp(value.data(), "LightmappedGeneric", value.size()) == 0 ||
    std::strncmp(value.data(), "LightmappedGeneric_DX9", value.size()) == 0;
}
} // namespace ldb_rt::material_decode
