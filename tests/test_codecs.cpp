// JB2 and IW44, exercised through the chunks of real documents.
//
// Neither codec has an encoder here, so these tests decode streams taken
// straight out of the corpus and check the properties a correct decode
// must have: plausible dimensions, ink where ink belongs, and a shared
// dictionary that actually supplies shapes.

#include "doctest.h"

#include <set>

#include "core/bytes.h"
#include "djvu/iff.h"
#include "djvu/iw44.h"
#include "djvu/jb2.h"
#include "fixtures.h"

using namespace vellum;
using namespace vellum::djvu;

namespace {

ByteSpan Span(const std::vector<uint8_t>& v) {
  return ByteSpan(v.data(), v.size());
}

// Finds the first chunk with `id` anywhere in a DjVu file, one level deep
// inside the component FORMs.
bool FindChunk(const std::vector<uint8_t>& file, const char* id,
               ByteSpan* out) {
  Chunk root = OpenDjVuFile(Span(file));
  ChunkReader top(root.body);
  Chunk c;
  while (top.Next(&c)) {
    if (c.is(id)) {
      *out = c.body;
      return true;
    }
    if (!c.is("FORM")) continue;
    ChunkReader inner(c.body);
    Chunk d;
    while (inner.Next(&d)) {
      if (d.is(id)) {
        *out = d.body;
        return true;
      }
    }
  }
  return false;
}

}  // namespace

TEST_CASE("A JB2 shape dictionary decodes to usable shapes") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> file =
      test::ReadFile(test::Sample(L"djvulibre-book-en.djvu"));
  REQUIRE_FALSE(file.empty());

  ByteSpan djbz;
  REQUIRE(FindChunk(file, "Djbz", &djbz));

  auto dict = DecodeJb2Dict(djbz, nullptr);
  REQUIRE(static_cast<bool>(dict));
  CHECK(dict->shape_count() > 50);        // a page's worth of glyphs
  CHECK(dict->inherited_count() == 0);

  // Every shape is a small, non-empty bitmap: these are character marks,
  // not page-sized images.
  int with_ink = 0;
  for (int i = 0; i < dict->shape_count(); ++i) {
    const Jb2Shape& s = dict->shape(i);
    CHECK(s.bits.rows() > 0);
    CHECK(s.bits.cols() > 0);
    CHECK(s.bits.rows() < 400);
    CHECK(s.bits.cols() < 400);
    for (int y = 0; y < s.bits.rows() && with_ink == i; ++y)
      for (int x = 0; x < s.bits.cols(); ++x)
        if (s.bits.row(y)[x]) {
          ++with_ink;
          break;
        }
  }
  CHECK(with_ink > dict->shape_count() / 2);

  // Index bounds are enforced rather than trusted.
  CHECK_THROWS_AS(dict->shape(-1), FormatError);
  CHECK_THROWS_AS(dict->shape(dict->shape_count()), FormatError);
}

TEST_CASE("A JB2 page mask composes to the size INFO declares") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> file =
      test::ReadFile(test::Sample(L"djvulibre-book-en.djvu"));
  REQUIRE_FALSE(file.empty());

  // The first page's mask needs the dictionary its INCL names.
  ByteSpan djbz, sjbz;
  REQUIRE(FindChunk(file, "Djbz", &djbz));
  REQUIRE(FindChunk(file, "Sjbz", &sjbz));

  auto dict = DecodeJb2Dict(djbz, nullptr);
  REQUIRE(static_cast<bool>(dict));
  auto image = DecodeJb2Image(sjbz, dict);
  REQUIRE(static_cast<bool>(image));

  CHECK(image->width > 500);
  CHECK(image->height > 500);
  CHECK_FALSE(image->blits.empty());
  // The page draws from the shared dictionary rather than resending shapes.
  CHECK(image->inherited_count() == dict->shape_count());

  Bitmap mask = image->Compose(1);
  CHECK(mask.cols() == image->width);
  CHECK(mask.rows() == image->height);

  long long ink = 0;
  for (int y = 0; y < mask.rows(); ++y)
    for (int x = 0; x < mask.cols(); ++x)
      if (mask.row(y)[x]) ++ink;
  // A page of text: some ink, but nowhere near all of it.
  CHECK(ink > 1000);
  CHECK(ink < static_cast<long long>(mask.rows()) * mask.cols() / 2);
}

TEST_CASE("Composing a mask subsampled gives coverage, not just on or off") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> file =
      test::ReadFile(test::Sample(L"djvulibre-book-en.djvu"));
  ByteSpan djbz, sjbz;
  REQUIRE(FindChunk(file, "Djbz", &djbz));
  REQUIRE(FindChunk(file, "Sjbz", &sjbz));
  auto image = DecodeJb2Image(sjbz, DecodeJb2Dict(djbz, nullptr));
  REQUIRE(static_cast<bool>(image));

  Bitmap small = image->Compose(4);
  CHECK(small.cols() == (image->width + 3) / 4);
  CHECK(small.rows() == (image->height + 3) / 4);
  // Reduced resolution accumulates counts, which is what anti-aliases the
  // ink instead of dropping it.
  CHECK(small.grays() == 4 * 4 + 1);

  std::set<int> levels;
  for (int y = 0; y < small.rows(); ++y)
    for (int x = 0; x < small.cols(); ++x) levels.insert(small.row(y)[x]);
  CHECK(levels.size() > 2);
}

TEST_CASE("IW44 decodes a background layer") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> file =
      test::ReadFile(test::Sample(L"lizard2002.djvu"));
  REQUIRE_FALSE(file.empty());

  // BG44 arrives as a sequence of chunks that progressively refine the
  // same image, so they are fed to one decoder in order.
  Iw44Decoder bg;
  int chunks = 0;
  Chunk root = OpenDjVuFile(Span(file));
  ChunkReader top(root.body);
  Chunk c;
  while (top.Next(&c)) {
    if (!c.is("FORM")) continue;
    ChunkReader inner(c.body);
    Chunk d;
    while (inner.Next(&d)) {
      if (!d.is("BG44")) continue;
      bg.AddChunk(d.body);
      ++chunks;
    }
    if (chunks) break;   // just the first page
  }
  REQUIRE(chunks > 0);
  REQUIRE(bg.ready());

  CHECK(bg.width() > 100);
  CHECK(bg.height() > 100);

  Pixmap pm = bg.ToPixmap();
  CHECK(pm.width() == bg.width());
  CHECK(pm.height() == bg.height());

  // A scanned page background is light overall rather than black, which is
  // the check that catches an inverted or mis-shifted reconstruction.
  long long total = 0;
  for (int y = 0; y < pm.height(); ++y) {
    const uint8_t* row = pm.row(y);
    for (int x = 0; x < pm.width(); ++x) total += row[x * 3 + 1];
  }
  const long long mean = total / (static_cast<long long>(pm.width()) * pm.height());
  CHECK(mean > 120);
}

TEST_CASE("An IW44 decoder with no data yields nothing") {
  Iw44Decoder empty;
  CHECK_FALSE(empty.ready());
  CHECK(empty.width() == 0);
  CHECK(empty.ToPixmap().empty());
}

TEST_CASE("IW44 rejects a chunk that arrives out of sequence") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> file =
      test::ReadFile(test::Sample(L"lizard2002.djvu"));
  ByteSpan bg44;
  REQUIRE(FindChunk(file, "BG44", &bg44));

  // The first chunk of a sequence carries serial 0; feeding the same one
  // twice must be refused rather than corrupting the image.
  Iw44Decoder d;
  d.AddChunk(bg44);
  CHECK_THROWS_AS(d.AddChunk(bg44), FormatError);
}

TEST_CASE("Garbage is refused by both codecs without crashing") {
  std::vector<uint8_t> junk;
  uint32_t x = 7;
  for (int i = 0; i < 512; ++i) {
    x = x * 1664525u + 1013904223u;
    junk.push_back(static_cast<uint8_t>(x >> 24));
  }

  // Either an error or an implausible-but-bounded result; never a crash.
  try {
    auto img = DecodeJb2Image(Span(junk), nullptr);
    if (img) CHECK(img->width >= 0);
  } catch (const FormatError&) {
  } catch (const std::out_of_range&) {
  }

  try {
    Iw44Decoder d;
    d.AddChunk(Span(junk));
    if (d.ready()) CHECK(d.width() >= 0);
  } catch (const FormatError&) {
  } catch (const std::out_of_range&) {
  }
}
