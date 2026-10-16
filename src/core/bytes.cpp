#include "core/bytes.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vellum {

std::vector<uint8_t> ReadWholeFile(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    throw FormatError("cannot open file");

  LARGE_INTEGER li{};
  if (!GetFileSizeEx(h, &li)) {
    CloseHandle(h);
    throw FormatError("cannot size file");
  }
  if (li.QuadPart > (1LL << 31)) {
    CloseHandle(h);
    throw FormatError("file too large");
  }

  std::vector<uint8_t> buf(static_cast<size_t>(li.QuadPart));
  size_t got = 0;
  while (got < buf.size()) {
    DWORD chunk = static_cast<DWORD>(
        (buf.size() - got) > (1u << 24) ? (1u << 24) : (buf.size() - got));
    DWORD read = 0;
    if (!ReadFile(h, buf.data() + got, chunk, &read, nullptr) || read == 0) {
      CloseHandle(h);
      throw FormatError("read failed");
    }
    got += read;
  }
  CloseHandle(h);
  return buf;
}

}  // namespace vellum
