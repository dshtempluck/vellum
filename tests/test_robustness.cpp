// Malformed input.
//
// Every document the viewer opens is untrusted. These tests mutate and
// truncate real files and require that each one either opens or reports an
// error -- never crashes, hangs, or allocates without bound. They are the
// reason the decoders bounds-check instead of trusting lengths.

#include "doctest.h"

#include <string>
#include <vector>

#include "core/bytes.h"
#include "djvu/document.h"
#include "doc/document.h"
#include "fixtures.h"

using namespace vellum;

namespace {

// Deterministic, so a failure can be reproduced from the seed alone.
uint32_t Next(uint32_t* state) {
  *state = *state * 1664525u + 1013904223u;
  return *state;
}

// Opens bytes as a DjVu document and renders a page, swallowing the
// errors a damaged file is entitled to produce.
void TryDjVu(const std::vector<uint8_t>& bytes) {
  try {
    auto doc = djvu::DjvuDocument::Open(bytes);
    if (!doc) return;
    const int pages = doc->page_count();
    CHECK(pages >= 0);
    CHECK(pages < 100000);
    if (pages > 0) {
      doc->geometry(0);
      doc->Text(0);
      doc->Render(0, Size{32, 32}, 0);
      doc->outline();
    }
  } catch (const FormatError&) {
  } catch (const std::out_of_range&) {
  } catch (const std::bad_alloc&) {
  } catch (const std::length_error&) {
  }
}

}  // namespace

TEST_CASE("Truncating a DjVu file at any point is survivable") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> original =
      test::ReadFile(test::Sample(L"lizard2002.djvu"));
  REQUIRE(original.size() > 1000);

  // Every prefix, at a coarse stride to keep the suite quick.
  for (size_t cut = 0; cut < original.size(); cut += original.size() / 40 + 1) {
    const std::vector<uint8_t> prefix(original.begin(),
                                      original.begin() + cut);
    TryDjVu(prefix);
  }
}

TEST_CASE("Flipping bytes in a DjVu file is survivable") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> original =
      test::ReadFile(test::Sample(L"lizard2002.djvu"));
  REQUIRE_FALSE(original.empty());

  uint32_t state = 20240517;
  for (int round = 0; round < 60; ++round) {
    std::vector<uint8_t> mutated = original;
    // A handful of byte flips per round, anywhere after the magic.
    for (int k = 0; k < 6; ++k) {
      const size_t at = 4 + Next(&state) % (mutated.size() - 4);
      mutated[at] = static_cast<uint8_t>(Next(&state));
    }
    TryDjVu(mutated);
  }
}

TEST_CASE("Corrupting the chunk lengths is survivable") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> original =
      test::ReadFile(test::Sample(L"lizard2007.djvu"));
  REQUIRE(original.size() > 100);

  // Lengths are what a naive reader would trust; make them absurd.
  for (size_t at = 8; at + 4 < original.size() && at < 4096; at += 97) {
    std::vector<uint8_t> mutated = original;
    mutated[at + 0] = 0x7f;
    mutated[at + 1] = 0xff;
    mutated[at + 2] = 0xff;
    mutated[at + 3] = 0xff;
    TryDjVu(mutated);
  }
}

TEST_CASE("A directory claiming impossible counts is refused") {
  // A file count far larger than the chunk could describe must not cause a
  // huge allocation before the data is seen to be missing.
  std::vector<uint8_t> dirm;
  dirm.push_back(0x81);            // bundled, version 1
  test::PutU16(&dirm, 0xffff);     // 65535 components, none of them present
  const std::vector<uint8_t> file =
      test::MakeForm("DJVM", test::MakeChunk("DIRM", dirm), true);
  CHECK_THROWS_AS(djvu::DjvuDocument::Open(file), FormatError);
}

TEST_CASE("A page with no INFO chunk is reported, not rendered as nothing") {
  std::vector<uint8_t> body = test::MakeChunk("TXTa", {0, 0, 0});
  const std::vector<uint8_t> file = test::MakeForm("DJVU", body, true);
  try {
    auto doc = djvu::DjvuDocument::Open(file);
    REQUIRE(static_cast<bool>(doc));
    // The page exists but cannot be measured, so it falls back to a
    // sensible box and renders blank with an explanation.
    CHECK(doc->page_count() == 1);
    RenderedPage out = doc->Render(0, Size{40, 40}, 0);
    CHECK_FALSE(out.complete);
    CHECK_FALSE(out.error.empty());
  } catch (const FormatError&) {
    // Refusing it outright is also acceptable.
  }
}

TEST_CASE("Deeply nested containers do not run away") {
  // Each level wraps the previous one; a reader without a bound would
  // recurse until the stack gave out.
  std::vector<uint8_t> inner = test::MakeChunk("INFO", {0, 1, 0, 1});
  for (int depth = 0; depth < 200; ++depth)
    inner = test::MakeChunk("FORM", [&] {
      std::vector<uint8_t> v = {'D', 'J', 'V', 'U'};
      v.insert(v.end(), inner.begin(), inner.end());
      return v;
    }());

  std::vector<uint8_t> file = {'A', 'T', '&', 'T'};
  file.insert(file.end(), inner.begin(), inner.end());
  TryDjVu(file);
}

TEST_CASE("Damaged images and PDFs are refused through dispatch") {
  test::TempDir dir;
  uint32_t state = 7777;

  // A BMP with a wrecked header.
  std::vector<uint8_t> bmp = test::MakeBmp(16, 16, 1, 2, 3);
  for (int k = 0; k < 10; ++k)
    bmp[2 + Next(&state) % 40] = static_cast<uint8_t>(Next(&state));
  try {
    OpenDocument(dir.Write(L"bad.bmp", bmp));
  } catch (const FormatError&) {
  }

  // A PDF with its cross-reference table scrambled.
  std::vector<uint8_t> pdf = test::MakePdf();
  for (int k = 0; k < 20; ++k)
    pdf[Next(&state) % pdf.size()] = static_cast<uint8_t>(Next(&state));
  try {
    auto doc = OpenDocument(dir.Write(L"bad.pdf", pdf));
    if (doc && doc->page_count() > 0) doc->Render(0, Size{16, 16}, 0);
  } catch (const FormatError&) {
  }
}

TEST_CASE("Empty and tiny files are refused everywhere") {
  test::TempDir dir;
  for (size_t n : {size_t{0}, size_t{1}, size_t{4}, size_t{15}}) {
    const std::vector<uint8_t> bytes(n, 0);
    CHECK_THROWS_AS(OpenDocument(dir.Write(L"small.bin", bytes)), FormatError);
  }
  CHECK_THROWS_AS(djvu::DjvuDocument::Open({}), FormatError);
}
