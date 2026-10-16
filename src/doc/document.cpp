#include "doc/document.h"

#include <algorithm>
#include <cstring>

#include "core/bytes.h"
#include "djvu/document.h"

#if VELLUM_HAVE_PDF
#include "pdf/document.h"
#endif

namespace vellum {
namespace {

bool HasPrefix(const std::vector<uint8_t>& b, const char* magic, size_t n) {
  return b.size() >= n && std::memcmp(b.data(), magic, n) == 0;
}

// PDF files are allowed up to 1024 bytes of junk before %PDF-.
bool LooksLikePdf(const std::vector<uint8_t>& b) {
  const size_t limit = std::min<size_t>(b.size(), 1024 + 5);
  for (size_t i = 0; i + 5 <= limit; ++i)
    if (std::memcmp(b.data() + i, "%PDF-", 5) == 0) return true;
  return false;
}

}  // namespace

std::unique_ptr<Document> OpenDocument(const std::wstring& path) {
  std::vector<uint8_t> bytes = ReadWholeFile(path);
  if (bytes.size() < 16) throw FormatError("file is too small to be a document");

  // Dispatch on content, not extension: a mislabelled file should still
  // open, and the two formats are trivially distinguishable.
  if (HasPrefix(bytes, "AT&T", 4))
    return djvu::DjvuDocument::Open(std::move(bytes));

  if (LooksLikePdf(bytes)) {
#if VELLUM_HAVE_PDF
    return pdf::PdfDocument::Open(std::move(bytes));
#else
    throw FormatError(
        "this build has no PDF support: configure with PDFIUM_ROOT");
#endif
  }

  throw FormatError("unrecognised file format (expected PDF or DjVu)");
}

}  // namespace vellum
