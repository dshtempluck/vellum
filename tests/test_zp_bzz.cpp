// The ZP arithmetic decoder and the BZZ byte compressor.
//
// There is no encoder in this project, so these tests do not round-trip.
// They pin down the properties that matter instead: the decoder is
// deterministic, it adapts, every entry point stays in range, and a stream
// that runs out produces an error rather than reading on forever. Real
// encoded streams come from the corpus.

#include "doctest.h"

#include <set>
#include <vector>

#include "core/bytes.h"
#include "djvu/bzz.h"
#include "djvu/iff.h"
#include "djvu/zp.h"
#include "fixtures.h"

using namespace vellum;
using namespace vellum::djvu;

namespace {

ByteSpan Span(const std::vector<uint8_t>& v) {
  return ByteSpan(v.data(), v.size());
}

// A fixed pseudo-random byte block. Any byte sequence is a decodable ZP
// stream -- arithmetic decoding always yields *some* bit sequence -- which
// is what makes these tests possible without an encoder.
std::vector<uint8_t> Noise(size_t n, uint32_t seed = 12345) {
  std::vector<uint8_t> out;
  out.reserve(n);
  uint32_t x = seed;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    out.push_back(static_cast<uint8_t>(x >> 24));
  }
  return out;
}

}  // namespace

TEST_CASE("The ZP state table is the one the format defines") {
  // State 0 is the even-odds start state; the tail is zero padding, since
  // the format defines states 0..250 in an array sized 256.
  CHECK(kZpTable[0].p == 0x8000);
  CHECK(kZpTable[0].m == 0x0000);
  CHECK(kZpTable[0].up == 84);
  CHECK(kZpTable[0].dn == 145);
  CHECK(kZpTable[250].p != 0);
  for (int i = 251; i < 256; ++i) {
    CHECK(kZpTable[i].p == 0);
    CHECK(kZpTable[i].up == 0);
    CHECK(kZpTable[i].dn == 0);
  }
  // Every transition stays inside the table.
  for (int i = 0; i <= 250; ++i) {
    CHECK(kZpTable[i].up <= 250);
    CHECK(kZpTable[i].dn <= 250);
  }
}

TEST_CASE("ZP decoding is deterministic") {
  const std::vector<uint8_t> data = Noise(512);

  auto run = [&] {
    ZpDecoder zp(Span(data));
    BitContext ctx = 0;
    std::vector<int> bits;
    for (int i = 0; i < 600; ++i) bits.push_back(zp.Decode(ctx));
    return bits;
  };

  const std::vector<int> first = run();
  CHECK(first == run());
  CHECK(first.size() == 600);
  // A decoder that returned a constant would be broken.
  const std::set<int> values(first.begin(), first.end());
  CHECK(values.size() == 2);
}

TEST_CASE("ZP contexts adapt as bits are decoded") {
  const std::vector<uint8_t> data = Noise(256, 99);
  ZpDecoder zp(Span(data));
  BitContext ctx = 0;
  std::set<BitContext> seen;
  for (int i = 0; i < 300; ++i) {
    zp.Decode(ctx);
    seen.insert(ctx);
  }
  // The context walks the state machine rather than sitting still.
  CHECK(seen.size() > 3);
}

TEST_CASE("ZP pass-through and fixed-probability decoders run") {
  const std::vector<uint8_t> data = Noise(256, 7);

  ZpDecoder a(Span(data));
  for (int i = 0; i < 100; ++i) {
    const int bit = a.DecodePassThrough();
    CHECK((bit == 0 || bit == 1));
  }

  ZpDecoder b(Span(data));
  for (int i = 0; i < 100; ++i) {
    const int bit = b.DecodeIw();
    CHECK((bit == 0 || bit == 1));
  }
}

TEST_CASE("ZP raw and binary reads stay inside their declared width") {
  const std::vector<uint8_t> data = Noise(512, 4242);

  ZpDecoder raw(Span(data));
  for (int bits = 1; bits <= 24; ++bits) {
    const uint32_t v = raw.DecodeRaw(bits);
    CHECK(v < (1u << bits));
  }

  ZpDecoder bin(Span(data));
  BitContext ctx[256] = {};
  for (int bits = 1; bits <= 8; ++bits) {
    const uint32_t v = bin.DecodeBinary(ctx, bits);
    CHECK(v < (1u << bits));
  }
}

TEST_CASE("A ZP stream that runs out eventually reports an error") {
  // Past the end the decoder is fed 0xff for a grace period, then refuses.
  // Without that limit a truncated file would decode forever.
  const std::vector<uint8_t> tiny = {0x00, 0x00};
  ZpDecoder zp(Span(tiny));
  BitContext ctx = 0;
  bool threw = false;
  try {
    for (int i = 0; i < 100000; ++i) zp.Decode(ctx);
  } catch (const FormatError&) {
    threw = true;
  }
  CHECK(threw);
}

TEST_CASE("An exhausted ZP stream terminates rather than looping forever") {
  // Past the end the decoder is fed 0xff for a grace period of 25 bytes,
  // then refuses. The grace is counted in bytes actually consumed, and the
  // fast path consumes none -- so an all-ones stream drives the context to
  // a highly skewed state where very few bits are spent, and the grace
  // takes millions of decodes to drain. That is bounded (tens of
  // milliseconds) rather than a hang, and the codecs above have their own
  // limits besides: JB2 caps its record count, BZZ its block size, IW44 its
  // slices. What matters here is that it stops.
  const ByteSpan nothing;
  ZpDecoder zp(nothing);
  BitContext ctx = 0;
  int decoded = 0;
  bool threw = false;
  try {
    for (int i = 0; i < 20000000; ++i, ++decoded) zp.Decode(ctx);
  } catch (const FormatError&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(decoded > 0);
  CHECK(decoded < 20000000);
}

TEST_CASE("BZZ rejects rather than crashes on data that is not BZZ") {
  // Garbage either throws or decodes to something; neither may crash, and
  // neither may hang.
  for (uint32_t seed : {1u, 2u, 3u, 99u}) {
    const std::vector<uint8_t> junk = Noise(64, seed);
    try {
      const std::vector<uint8_t> out = BzzDecompress(Span(junk));
      CHECK(out.size() < 16u * 1024 * 1024);
    } catch (const FormatError&) {
      // Expected for most inputs.
    }
  }
  // An empty stream decodes to nothing rather than faulting.
  try {
    CHECK(BzzDecompress(ByteSpan{}).empty());
  } catch (const FormatError&) {
  }
}

TEST_CASE("BZZ decompresses a real document directory") {
  if (!test::HasCorpus()) return;
  const std::vector<uint8_t> file =
      test::ReadFile(test::Sample(L"djvu3spec.djvu"));
  REQUIRE_FALSE(file.empty());

  Chunk root = OpenDjVuFile(Span(file));
  ChunkReader r(root.body);
  Chunk c;
  bool checked = false;
  while (r.Next(&c)) {
    if (!c.is("DIRM")) continue;
    // The directory starts with a flags byte and a file count, then the
    // bundled offsets; everything after that is BZZ.
    ByteReader br(c.body);
    br.u8();
    const int files = br.u16();
    CHECK(files > 70);
    for (int i = 0; i < files; ++i) br.u32();

    const std::vector<uint8_t> meta =
        BzzDecompress(c.body.subspan(br.pos(), c.body.size() - br.pos()));
    // Sizes, flags and a name block for every component.
    CHECK(meta.size() > static_cast<size_t>(files) * 4);
    checked = true;
  }
  CHECK(checked);
}
