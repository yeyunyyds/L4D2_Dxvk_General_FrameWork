#pragma once
// CRT adapters for the unmodified pinned SDK on Linux. Bounds failures abort;
// SDK assertions stay enabled. Windows builds use the platform CRT instead.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <locale>
#ifndef _WIN32
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define sprintf_s std::snprintf
inline int wcscpy_s(wchar_t *dst, size_t count, const wchar_t *src) {
  if (!src || std::wcslen(src) >= count)
    std::abort();
  std::wcscpy(dst, src);
  return 0;
}
template <size_t N> int wcscpy_s(wchar_t (&dst)[N], const wchar_t *src) {
  return wcscpy_s(dst, N, src);
}
inline int strcpy_s(char *dst, size_t count, const char *src) {
  if (!src || std::strlen(src) >= count)
    std::abort();
  std::strcpy(dst, src);
  return 0;
}
template <size_t N> int strcpy_s(char (&dst)[N], const char *src) {
  return strcpy_s(dst, N, src);
}
inline int wcstombs_s(size_t *used, char *dst, size_t size, const wchar_t *src,
                      size_t count) {
  // SDK labels are ASCII. Truncate only this diagnostic label, never resource
  // data.
  size_t i = 0;
  for (; i + 1 < size && i < count && src[i]; ++i)
    dst[i] = src[i] < 128 ? char(src[i]) : '?';
  if (!size)
    std::abort();
  dst[i] = 0;
  if (used)
    *used = i + 1;
  return 0;
}
#endif
