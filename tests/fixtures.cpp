#include "fixtures.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace vellum::test {
namespace {

std::wstring Widen(const char* s) {
  std::wstring w;
  while (*s) w.push_back(static_cast<wchar_t>(*s++));
  return w;
}

}  // namespace

std::wstring SampleDir() {
#ifdef VELLUM_SOURCE_DIR
  return Widen(VELLUM_SOURCE_DIR) + L"\\samples";
#else
  return L"samples";
#endif
}

std::wstring Sample(const wchar_t* name) {
  return SampleDir() + L"\\" + name;
}

bool HasCorpus() {
  // One representative file stands in for the set; fetch-samples.ps1
  // downloads them together.
  const DWORD attrs = GetFileAttributesW(Sample(L"djvu3spec.djvu").c_str());
  return attrs != INVALID_FILE_ATTRIBUTES;
}

std::vector<uint8_t> ReadFile(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER li{};
  GetFileSizeEx(h, &li);
  std::vector<uint8_t> out(static_cast<size_t>(li.QuadPart));
  DWORD got = 0;
  // Qualified: this function's own name would otherwise shadow the Win32
  // ReadFile it is trying to call.
  if (!out.empty())
    ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
  CloseHandle(h);
  out.resize(got);
  return out;
}

TempDir::TempDir() {
  wchar_t base[MAX_PATH]{};
  GetTempPathW(MAX_PATH, base);
  for (int attempt = 0; attempt < 64; ++attempt) {
    wchar_t name[MAX_PATH]{};
    swprintf(name, MAX_PATH, L"%svellum_test_%lu_%d", base,
             GetCurrentProcessId(), attempt);
    if (CreateDirectoryW(name, nullptr)) {
      path_ = name;
      return;
    }
  }
}

TempDir::~TempDir() {
  if (path_.empty()) return;
  // Remove the contents, then the directory. One level is enough: nothing
  // here creates subdirectories.
  WIN32_FIND_DATAW find{};
  HANDLE h = FindFirstFileW((path_ + L"\\*").c_str(), &find);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      DeleteFileW((path_ + L"\\" + find.cFileName).c_str());
    } while (FindNextFileW(h, &find));
    FindClose(h);
  }
  RemoveDirectoryW(path_.c_str());
}

std::wstring TempDir::Write(const wchar_t* name,
                            const std::vector<uint8_t>& bytes) {
  const std::wstring full = path_ + L"\\" + name;
  HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::wstring();
  DWORD written = 0;
  if (!bytes.empty())
    ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                nullptr);
  CloseHandle(h);
  return full;
}

// --- synthetic documents ---------------------------------------------------

namespace {

void PutU16le(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xff));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void PutU32le(std::vector<uint8_t>* out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

}  // namespace

std::vector<uint8_t> MakeBmp(int width, int height, uint8_t r, uint8_t g,
                             uint8_t b) {
  // Rows are bottom-up and padded to four bytes, which is also a useful
  // reminder that the viewer's own rasters pad the same way.
  const size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~size_t{3};
  const size_t pixels = stride * static_cast<size_t>(height);

  std::vector<uint8_t> out;
  out.push_back('B');
  out.push_back('M');
  PutU32le(&out, static_cast<uint32_t>(54 + pixels));
  PutU32le(&out, 0);
  PutU32le(&out, 54);

  PutU32le(&out, 40);                                   // header size
  PutU32le(&out, static_cast<uint32_t>(width));
  PutU32le(&out, static_cast<uint32_t>(height));
  PutU16le(&out, 1);                                    // planes
  PutU16le(&out, 24);                                   // bits per pixel
  PutU32le(&out, 0);                                    // BI_RGB
  PutU32le(&out, static_cast<uint32_t>(pixels));
  PutU32le(&out, 2835);                                 // 72 dpi, x
  PutU32le(&out, 2835);                                 // 72 dpi, y
  PutU32le(&out, 0);
  PutU32le(&out, 0);

  const size_t start = out.size();
  out.resize(start + pixels, 0);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = out.data() + start + stride * static_cast<size_t>(y);
    for (int x = 0; x < width; ++x) {
      row[x * 3 + 0] = b;
      row[x * 3 + 1] = g;
      row[x * 3 + 2] = r;
    }
  }
  return out;
}

std::vector<uint8_t> MakePdf(double width_pt, double height_pt,
                             bool fill_red) {
  char content[128]{};
  if (fill_red)
    std::snprintf(content, sizeof(content), "1 0 0 rg 0 0 %.0f %.0f re f\n",
                  width_pt, height_pt);
  else
    std::snprintf(content, sizeof(content), "0 0 0 RG 1 w 10 10 m 100 100 l S\n");
  const size_t content_len = std::strlen(content);

  char page[256]{};
  std::snprintf(page, sizeof(page),
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] "
                "/Contents 4 0 R >>",
                width_pt, height_pt);

  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      page,
      "<< /Length " + std::to_string(content_len) + " >>\nstream\n" +
          content + "endstream",
  };

  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
  out += "0000000000 65535 f \n";
  for (size_t off : offsets) {
    char line[32]{};
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", off);
    out += line;
  }
  out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";

  return std::vector<uint8_t>(out.begin(), out.end());
}

std::vector<uint8_t> MakePdfWithOutline() {
  // Three pages so bookmark destinations differ, an outline tree two deep,
  // and an Info dictionary carrying a title.
  const std::string content = "BT /F1 12 Tf 72 700 Td (page) Tj ET\n";

  std::vector<std::string> objects = {
      // 1 catalog
      "<< /Type /Catalog /Pages 2 0 R /Outlines 6 0 R >>",
      // 2 pages
      "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
      // 3..5 pages
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 10 0 R >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 10 0 R >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 10 0 R >>",
      // 6 outline root
      "<< /Type /Outlines /First 7 0 R /Last 9 0 R /Count 3 >>",
      // 7 first top-level item, with a child
      "<< /Title (Chapter One) /Parent 6 0 R /Next 9 0 R /First 8 0 R "
      "/Last 8 0 R /Count 1 /Dest [3 0 R /Fit] >>",
      // 8 child of 7
      "<< /Title (Section 1.1) /Parent 7 0 R /Dest [4 0 R /Fit] >>",
      // 9 second top-level item, reached through an action rather than a
      // direct destination, which is the other path the walker handles
      "<< /Title (Chapter Two) /Parent 6 0 R /Prev 7 0 R "
      "/A << /S /GoTo /D [5 0 R /Fit] >> >>",
      // 10 shared content stream
      "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
          content + "endstream",
      // 11 info dictionary
      "<< /Title (Vellum Outline Fixture) /Author (tests) >>",
  };

  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
  out += "0000000000 65535 f \n";
  for (size_t off : offsets) {
    char line[32]{};
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", off);
    out += line;
  }
  out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R /Info 11 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return std::vector<uint8_t>(out.begin(), out.end());
}

std::vector<uint8_t> MakeTiff(int width, int height, int orientation) {
  // Baseline grayscale, one strip, uncompressed. Every tag value fits in
  // the entry itself, so there is no out-of-line data to place.
  const int kEntries = 10;
  const size_t ifd_at = 8;
  const size_t ifd_size = 2 + static_cast<size_t>(kEntries) * 12 + 4;
  const size_t pixels_at = ifd_at + ifd_size;
  const size_t pixel_bytes = static_cast<size_t>(width) * height;

  std::vector<uint8_t> out;
  out.push_back('I');
  out.push_back('I');
  PutU16le(&out, 42);
  PutU32le(&out, static_cast<uint32_t>(ifd_at));

  PutU16le(&out, kEntries);
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                   uint32_t value) {
    PutU16le(&out, tag);
    PutU16le(&out, type);
    PutU32le(&out, count);
    // A SHORT sits in the low half of the value field; a LONG fills it.
    if (type == 3) {
      PutU16le(&out, value);
      PutU16le(&out, 0);
    } else {
      PutU32le(&out, value);
    }
  };
  // Tags must appear in ascending order.
  entry(256, 4, 1, static_cast<uint32_t>(width));        // ImageWidth
  entry(257, 4, 1, static_cast<uint32_t>(height));       // ImageLength
  entry(258, 3, 1, 8);                                   // BitsPerSample
  entry(259, 3, 1, 1);                                   // Compression: none
  entry(262, 3, 1, 1);                                   // BlackIsZero
  entry(273, 4, 1, static_cast<uint32_t>(pixels_at));    // StripOffsets
  entry(274, 3, 1, static_cast<uint32_t>(orientation));  // Orientation
  entry(277, 3, 1, 1);                                   // SamplesPerPixel
  entry(278, 4, 1, static_cast<uint32_t>(height));       // RowsPerStrip
  entry(279, 4, 1, static_cast<uint32_t>(pixel_bytes));  // StripByteCounts
  PutU32le(&out, 0);                                     // no next IFD

  // A gradient, so a rotation is visible in the pixels rather than only in
  // the dimensions.
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      out.push_back(static_cast<uint8_t>((x * 8 + y * 2) & 0xff));
  return out;
}

void PutU16(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out->push_back(static_cast<uint8_t>(v & 0xff));
}

void PutU24(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out->push_back(static_cast<uint8_t>(v & 0xff));
}

void PutU32(std::vector<uint8_t>* out, uint32_t v) {
  for (int i = 3; i >= 0; --i)
    out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

std::vector<uint8_t> MakeChunk(const char id[5],
                               const std::vector<uint8_t>& body) {
  std::vector<uint8_t> out;
  out.insert(out.end(), id, id + 4);
  PutU32(&out, static_cast<uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  if (body.size() & 1) out.push_back(0);  // chunks are even-aligned
  return out;
}

std::vector<uint8_t> MakeForm(const char form_id[5],
                              const std::vector<uint8_t>& body,
                              bool with_magic) {
  std::vector<uint8_t> inner(form_id, form_id + 4);
  inner.insert(inner.end(), body.begin(), body.end());

  std::vector<uint8_t> out;
  if (with_magic) {
    const char magic[] = "AT&T";
    out.insert(out.end(), magic, magic + 4);
  }
  const std::vector<uint8_t> form = MakeChunk("FORM", inner);
  out.insert(out.end(), form.begin(), form.end());
  return out;
}

}  // namespace vellum::test
