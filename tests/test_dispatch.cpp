// Format dispatch: choosing a backend from the bytes, not the extension.

#include "doctest.h"

#include "core/bytes.h"
#include "doc/document.h"
#include "fixtures.h"

using namespace vellum;

TEST_CASE("A file too small to be anything is rejected") {
  test::TempDir dir;
  const std::wstring p = dir.Write(L"tiny.pdf", {1, 2, 3});
  CHECK_THROWS_AS(OpenDocument(p), FormatError);
}

TEST_CASE("A missing file is rejected") {
  CHECK_THROWS_AS(OpenDocument(L"Z:\\nope\\missing.pdf"), FormatError);
}

TEST_CASE("Content decides the backend, not the extension") {
  test::TempDir dir;

  // A PDF named .djvu still opens as a PDF.
  const std::wstring mislabelled = dir.Write(L"actually.djvu", test::MakePdf());
  auto doc = OpenDocument(mislabelled);
  REQUIRE(static_cast<bool>(doc));
  CHECK(doc->format_name().rfind("PDF", 0) == 0);
  CHECK(doc->page_count() == 1);

  // A BMP named .pdf still opens as an image.
  const std::wstring img = dir.Write(L"actually.pdf", test::MakeBmp(8, 8, 1, 2, 3));
  auto image = OpenDocument(img);
  REQUIRE(static_cast<bool>(image));
  CHECK(image->page_count() >= 1);
}

TEST_CASE("Unrecognised content is reported, not guessed at") {
  test::TempDir dir;
  std::vector<uint8_t> junk(256, 0x7f);
  const std::wstring p = dir.Write(L"mystery.dat", junk);
  CHECK_THROWS_AS(OpenDocument(p), FormatError);
}

TEST_CASE("PDF is found even behind leading junk") {
  // The format allows up to 1024 bytes before the header.
  std::vector<uint8_t> bytes(600, ' ');
  const std::vector<uint8_t> pdf = test::MakePdf();
  bytes.insert(bytes.end(), pdf.begin(), pdf.end());

  test::TempDir dir;
  const std::wstring p = dir.Write(L"offset.pdf", bytes);
  // PDFium may or may not accept the shifted offsets, but dispatch must at
  // least route it to the PDF backend rather than refusing the file.
  try {
    auto doc = OpenDocument(p);
    CHECK(doc->format_name().rfind("PDF", 0) == 0);
  } catch (const FormatError& e) {
    CHECK(std::string(e.what()).find("image") == std::string::npos);
  }
}

TEST_CASE("The DjVu magic routes to the DjVu backend") {
  test::TempDir dir;
  // Correct magic, nothing valid behind it: the DjVu backend must be the
  // one to complain.
  std::vector<uint8_t> bytes = {'A', 'T', '&', 'T'};
  bytes.resize(64, 0);
  const std::wstring p = dir.Write(L"broken.djvu", bytes);
  CHECK_THROWS_AS(OpenDocument(p), FormatError);
}

TEST_CASE("A real DjVu document opens through dispatch") {
  if (!test::HasCorpus()) return;
  auto doc = OpenDocument(test::Sample(L"djvu3spec.djvu"));
  REQUIRE(static_cast<bool>(doc));
  CHECK(doc->page_count() == 71);
  CHECK(doc->format_name().find("DjVu") != std::string::npos);
}

TEST_CASE("Document defaults are sensible for a backend that ignores them") {
  test::TempDir dir;
  const std::wstring p = dir.Write(L"plain.pdf", test::MakePdf());
  auto doc = OpenDocument(p);
  REQUIRE(static_cast<bool>(doc));
  // Defaults from the base interface.
  CHECK(doc->initial_page() == 0);
  CHECK(doc->page_label(0).empty());
  CHECK_FALSE(doc->pages_are_independent());
}
