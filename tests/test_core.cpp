// Byte readers, rasters and geometry.

#include "doctest.h"

#include "core/bitmap.h"
#include "core/bytes.h"
#include "core/geometry.h"
#include "fixtures.h"

using namespace vellum;

TEST_CASE("ByteSpan refuses to read past its end") {
  const uint8_t data[] = {1, 2, 3, 4};
  ByteSpan s(data, 4);

  CHECK(s.size() == 4);
  CHECK_FALSE(s.empty());
  CHECK(s[0] == 1);
  CHECK(s[3] == 4);
  CHECK_THROWS_AS(s[4], std::out_of_range);

  CHECK(s.subspan(1, 2).size() == 2);
  CHECK(s.subspan(4, 0).size() == 0);      // empty at the very end is fine
  CHECK_THROWS_AS(s.subspan(0, 5), std::out_of_range);
  CHECK_THROWS_AS(s.subspan(5, 0), std::out_of_range);
  // Overflow must not wrap into an apparently valid range.
  CHECK_THROWS_AS(s.subspan(2, SIZE_MAX), std::out_of_range);

  CHECK(ByteSpan().empty());
}

TEST_CASE("ByteReader reads big-endian and reports exhaustion") {
  const uint8_t data[] = {0x12, 0x34, 0x56, 0x78, 'h', 'i', 0, 0xff};
  ByteReader r(ByteSpan(data, sizeof(data)));

  CHECK(r.remaining() == 8);
  CHECK(r.u8() == 0x12);
  CHECK(r.u16() == 0x3456);
  CHECK(r.pos() == 3);

  r.seek(0);
  CHECK(r.u32() == 0x12345678u);
  CHECK(r.cstr() == "hi");
  CHECK(r.u8() == 0xff);
  CHECK(r.eof());
  CHECK_THROWS_AS(r.u8(), std::out_of_range);

  r.seek(0);
  CHECK(r.u24() == 0x123456u);
  CHECK_THROWS_AS(r.seek(9), std::out_of_range);

  // A string with no terminator runs off the end rather than reading on.
  const uint8_t unterminated[] = {'a', 'b'};
  ByteReader r2(ByteSpan(unterminated, 2));
  CHECK_THROWS_AS(r2.cstr(), std::out_of_range);
}

TEST_CASE("ByteReader::take hands out sub-spans and advances") {
  const uint8_t data[] = {1, 2, 3, 4, 5};
  ByteReader r(ByteSpan(data, 5));
  ByteSpan a = r.take(2);
  CHECK(a.size() == 2);
  CHECK(a[1] == 2);
  CHECK(r.pos() == 2);
  CHECK_THROWS_AS(r.take(99), std::out_of_range);
}

TEST_CASE("ReadWholeFile rejects a missing file") {
  CHECK_THROWS_AS(ReadWholeFile(L"Z:\\vellum\\definitely\\not\\here.bin"),
                  FormatError);
}

TEST_CASE("ReadWholeFile round-trips bytes") {
  test::TempDir dir;
  REQUIRE_FALSE(dir.path().empty());
  const std::vector<uint8_t> written = {0, 1, 2, 250, 251, 255};
  const std::wstring path = dir.Write(L"blob.bin", written);
  CHECK(ReadWholeFile(path) == written);

  // An empty file is readable and yields nothing.
  const std::wstring empty = dir.Write(L"empty.bin", {});
  CHECK(ReadWholeFile(empty).empty());
}

TEST_CASE("Bitmap borders read as zero outside the image") {
  Bitmap bm(4, 5, 2);
  CHECK(bm.rows() == 4);
  CHECK(bm.cols() == 5);
  CHECK(bm.border() == 2);
  CHECK_FALSE(bm.empty());
  CHECK(bm.grays() == 2);

  bm.Fill(1);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 5; ++x) CHECK(bm.row(y)[x] == 1);

  // The border is untouched by Fill and reads as zero, which is what lets
  // the JB2 context windows run without bounds checks.
  CHECK(bm.row(0)[-1] == 0);
  CHECK(bm.row(0)[5] == 0);
  CHECK(bm.row(-1)[0] == 0);
  CHECK(bm.row(4)[0] == 0);

  CHECK(Bitmap().empty());
}

TEST_CASE("Bitmap::MinBorder grows without disturbing pixels") {
  Bitmap bm(3, 3, 1);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x)
      bm.row(y)[x] = static_cast<uint8_t>(y * 3 + x);

  bm.MinBorder(0);                 // never shrinks
  CHECK(bm.border() == 1);
  bm.MinBorder(5);
  CHECK(bm.border() == 5);
  CHECK(bm.rows() == 3);
  CHECK(bm.cols() == 3);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x) CHECK(bm.row(y)[x] == y * 3 + x);
  CHECK(bm.row(0)[-5] == 0);
}

TEST_CASE("Bitmap::Blit clips at every edge") {
  Bitmap dst(4, 4, 0);
  Bitmap src(2, 2, 0);
  src.Fill(1);

  dst.Blit(src, 1, 1);
  CHECK(dst.row(1)[1] == 1);
  CHECK(dst.row(2)[2] == 1);
  CHECK(dst.row(0)[0] == 0);

  // Entirely outside in each direction: nothing is written, nothing faults.
  Bitmap clean(4, 4, 0);
  clean.Blit(src, -10, 0);
  clean.Blit(src, 10, 0);
  clean.Blit(src, 0, -10);
  clean.Blit(src, 0, 10);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) CHECK(clean.row(y)[x] == 0);

  // Straddling an edge writes only the part that lands inside.
  Bitmap edge(4, 4, 0);
  edge.Blit(src, -1, -1);
  CHECK(edge.row(0)[0] == 1);
  CHECK(edge.row(1)[1] == 0);
}

TEST_CASE("Bitmap::BlitSubsampled accumulates coverage") {
  Bitmap dst(2, 2, 0);
  Bitmap src(4, 4, 0);
  src.Fill(1);

  // Every 2x2 block of the source maps to one destination pixel, so each
  // gets a count of four. That count is what anti-aliases a reduced mask.
  dst.BlitSubsampled(src, 0, 0, 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) CHECK(dst.row(y)[x] == 4);

  // A subsample of one is an ordinary blit.
  Bitmap plain(4, 4, 0);
  plain.BlitSubsampled(src, 0, 0, 1);
  CHECK(plain.row(0)[0] == 1);

  // Negative placement is dropped rather than wrapping.
  Bitmap neg(2, 2, 0);
  neg.BlitSubsampled(src, -8, -8, 2);
  CHECK(neg.row(0)[0] == 0);
}

TEST_CASE("Pixmap pads rows to four bytes and stores BGR") {
  // The padding is why a page is not sheared when GDI blits it: a DIB's
  // stride is always rounded up, and GDI derives it from the width alone.
  for (int w : {1, 2, 3, 4, 5, 7, 16, 17}) {
    Pixmap pm(w, 2);
    CHECK(pm.stride() % 4 == 0);
    CHECK(pm.stride() >= static_cast<size_t>(w) * 3);
    CHECK(pm.width() == w);
    CHECK(pm.height() == 2);
    // Consecutive rows are exactly one stride apart.
    CHECK(pm.row(1) - pm.row(0) == static_cast<ptrdiff_t>(pm.stride()));
  }

  Pixmap pm(2, 1);
  pm.Fill(0x11, 0x22, 0x33);  // blue, green, red
  CHECK(pm.row(0)[0] == 0x11);
  CHECK(pm.row(0)[1] == 0x22);
  CHECK(pm.row(0)[2] == 0x33);

  CHECK(Pixmap().empty());
}

TEST_CASE("Rect arithmetic") {
  const Rect a = Rect::FromXYWH(0, 0, 10, 10);
  const Rect b = Rect::FromXYWH(5, 5, 10, 10);

  CHECK(a.width() == 10);
  CHECK(a.height() == 10);
  CHECK_FALSE(a.empty());
  CHECK(Rect().empty());

  CHECK(a.Contains(0, 0));
  CHECK(a.Contains(9, 9));
  CHECK_FALSE(a.Contains(10, 10));   // half-open

  const Rect hit = a.Intersect(b);
  CHECK(hit.x0 == 5);
  CHECK(hit.x1 == 10);
  CHECK(a.Intersects(b));

  const Rect apart = Rect::FromXYWH(100, 100, 1, 1);
  CHECK(a.Intersect(apart).empty());
  CHECK_FALSE(a.Intersects(apart));

  const Rect u = a.Union(b);
  CHECK(u.x0 == 0);
  CHECK(u.x1 == 15);
  // Union with an empty rectangle is identity, in both directions.
  CHECK(a.Union(Rect()).x1 == 10);
  CHECK(Rect().Union(a).x1 == 10);

  CHECK(a.Offset(3, 4).x0 == 3);
  CHECK(a.Offset(3, 4).y1 == 14);
  CHECK(a.Inflate(2).x0 == -2);
  CHECK(a.Inflate(2).x1 == 12);
}

TEST_CASE("Size and RectF emptiness") {
  CHECK(Size{}.empty());
  CHECK(Size{0, 5}.empty());
  CHECK(Size{5, 0}.empty());
  CHECK_FALSE((Size{1, 1}.empty()));

  CHECK(RectF{}.empty());
  CHECK(RectF{0, 0, 1, 1}.empty() == false);
  CHECK(RectF{1, 0, 0, 1}.empty());
}

TEST_CASE("ReadWholeFile handles a file larger than one read chunk") {
  // Reads are issued in 16 MB chunks, so anything larger exercises the
  // loop that reassembles them. A short read there would silently
  // truncate a large document.
  test::TempDir dir;
  REQUIRE_FALSE(dir.path().empty());

  const size_t size = (16u << 20) + 4096;   // just over one chunk
  std::vector<uint8_t> written(size);
  // A pattern rather than zeros, so a misassembled read is detectable.
  for (size_t i = 0; i < size; ++i)
    written[i] = static_cast<uint8_t>((i * 31 + (i >> 13)) & 0xff);

  const std::wstring path = dir.Write(L"big.bin", written);
  REQUIRE_FALSE(path.empty());

  const std::vector<uint8_t> read_back = ReadWholeFile(path);
  REQUIRE(read_back.size() == written.size());
  CHECK(read_back == written);
  // Spot-check the boundary, which is where a chunking mistake shows.
  CHECK(read_back[(16u << 20) - 1] == written[(16u << 20) - 1]);
  CHECK(read_back[(16u << 20)] == written[(16u << 20)]);
}
