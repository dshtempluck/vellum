// The IFF container: chunk walking, padding, and malformed input.

#include "doctest.h"

#include "core/bytes.h"
#include "djvu/iff.h"
#include "fixtures.h"

using namespace vellum;
using namespace vellum::djvu;
using test::MakeChunk;
using test::MakeForm;

namespace {

ByteSpan Span(const std::vector<uint8_t>& v) {
  return ByteSpan(v.data(), v.size());
}

}  // namespace

TEST_CASE("Chunk identity comparisons") {
  std::vector<uint8_t> body = {1, 2, 3};
  std::vector<uint8_t> raw = MakeChunk("INFO", body);
  ChunkReader r(Span(raw));
  Chunk c;
  REQUIRE(r.Next(&c));
  CHECK(c.is("INFO"));
  CHECK_FALSE(c.is("DIRM"));
  CHECK_FALSE(c.is_form("DJVU"));   // not a composite chunk
  CHECK(c.body.size() == 3);
  CHECK(c.offset == 0);
  CHECK_FALSE(r.Next(&c));          // exactly one chunk
}

TEST_CASE("Odd-length chunks are padded to an even boundary") {
  // Three bytes of body means one pad byte, and the next chunk has to
  // start after it rather than inside it.
  std::vector<uint8_t> raw = MakeChunk("AAAA", {1, 2, 3});
  const std::vector<uint8_t> second = MakeChunk("BBBB", {9});
  raw.insert(raw.end(), second.begin(), second.end());

  ChunkReader r(Span(raw));
  Chunk c;
  REQUIRE(r.Next(&c));
  CHECK(c.is("AAAA"));
  REQUIRE(r.Next(&c));
  CHECK(c.is("BBBB"));
  CHECK(c.body.size() == 1);
  CHECK(c.body[0] == 9);
  CHECK_FALSE(r.Next(&c));
}

TEST_CASE("A chunk ending exactly on its pad byte is still well formed") {
  // The pad byte may be absent at end of file; that must not be an error.
  std::vector<uint8_t> raw = MakeChunk("AAAA", {1, 2, 3});
  raw.pop_back();  // drop the trailing pad
  ChunkReader r(Span(raw));
  Chunk c;
  REQUIRE(r.Next(&c));
  CHECK(c.is("AAAA"));
  CHECK_FALSE(r.Next(&c));
}

TEST_CASE("Composite chunks expose their secondary id") {
  const std::vector<uint8_t> inner = MakeChunk("INFO", {0, 1});
  const std::vector<uint8_t> raw = MakeForm("DJVU", inner, false);

  ChunkReader r(Span(raw));
  Chunk form;
  REQUIRE(r.Next(&form));
  CHECK(form.is("FORM"));
  CHECK(form.is_form("DJVU"));
  CHECK_FALSE(form.is_form("DJVM"));

  // The secondary id is not part of the body handed back.
  ChunkReader inner_reader(form.body);
  Chunk c;
  REQUIRE(inner_reader.Next(&c));
  CHECK(c.is("INFO"));
  CHECK(c.body.size() == 2);
}

TEST_CASE("A chunk claiming more than the container holds is rejected") {
  std::vector<uint8_t> raw = MakeChunk("AAAA", {1, 2, 3, 4});
  raw[7] = 0xff;  // length byte: now far larger than the body
  ChunkReader r(Span(raw));
  Chunk c;
  CHECK_THROWS_AS(r.Next(&c), FormatError);
}

TEST_CASE("A composite chunk too short to hold its secondary id is rejected") {
  // FORM with a declared length of 2: not enough for the four-byte id.
  std::vector<uint8_t> raw;
  const char id[] = "FORM";
  raw.insert(raw.end(), id, id + 4);
  test::PutU32(&raw, 2);
  raw.push_back('D');
  raw.push_back('J');
  ChunkReader r(Span(raw));
  Chunk c;
  CHECK_THROWS_AS(r.Next(&c), FormatError);
}

TEST_CASE("A truncated header ends the walk instead of faulting") {
  // Fewer than eight bytes left cannot be a header; that is the end.
  const std::vector<uint8_t> raw = {'A', 'A', 'A', 'A', 0, 0};
  ChunkReader r(Span(raw));
  Chunk c;
  CHECK_FALSE(r.Next(&c));
}

TEST_CASE("OpenDjVuFile checks the magic and the root chunk") {
  const std::vector<uint8_t> inner = MakeChunk("INFO", {0, 1, 2, 3});
  const std::vector<uint8_t> good = MakeForm("DJVU", inner, true);
  Chunk root = OpenDjVuFile(Span(good));
  CHECK(root.is_form("DJVU"));

  // Too short to hold even a header.
  const std::vector<uint8_t> tiny = {'A', 'T', '&', 'T'};
  CHECK_THROWS_AS(OpenDjVuFile(Span(tiny)), FormatError);

  // Right size, wrong magic.
  std::vector<uint8_t> wrong_magic = good;
  wrong_magic[0] = 'X';
  CHECK_THROWS_AS(OpenDjVuFile(Span(wrong_magic)), FormatError);

  // Magic present, but the root is not a FORM.
  std::vector<uint8_t> not_form;
  const char magic[] = "AT&T";
  not_form.insert(not_form.end(), magic, magic + 4);
  const std::vector<uint8_t> plain = MakeChunk("INFO", {1, 2, 3, 4});
  not_form.insert(not_form.end(), plain.begin(), plain.end());
  CHECK_THROWS_AS(OpenDjVuFile(Span(not_form)), FormatError);
}

TEST_CASE("Every chunk of a real DjVu file is walkable") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> file =
      test::ReadFile(test::Sample(L"djvu3spec.djvu"));
  REQUIRE_FALSE(file.empty());

  Chunk root = OpenDjVuFile(Span(file));
  REQUIRE(root.is_form("DJVM"));

  int forms = 0, dirm = 0, navm = 0;
  ChunkReader r(root.body);
  Chunk c;
  while (r.Next(&c)) {
    if (c.is("DIRM")) ++dirm;
    else if (c.is("NAVM")) ++navm;
    else if (c.is("FORM")) ++forms;
    // Every chunk body must lie inside the file.
    CHECK(c.body.data() >= file.data());
    CHECK(c.body.data() + c.body.size() <= file.data() + file.size());
  }
  CHECK(dirm == 1);
  CHECK(navm == 1);
  CHECK(forms > 70);   // 71 pages plus shared dictionaries
}
