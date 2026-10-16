#include "djvu/bzz.h"

#include <cstring>

#include "djvu/zp.h"

namespace vellum::djvu {
namespace {

// Block size ceiling (in KB) and the small frequency window the MTF stage
// keeps exactly; both are fixed by the format.
constexpr int kMaxBlockKb = 4096;
constexpr int kFreqMax = 4;
constexpr int kCtxIds = 3;
constexpr int kNumContexts = 300;

// Decodes one BZZ block into `out`, appending to it. Returns false on the
// terminating zero-length block.
bool DecodeBlock(ZpDecoder& zp, BitContext* ctx, std::vector<uint8_t>& out) {
  const uint32_t size = zp.DecodeRaw(24);
  if (size == 0) return false;
  if (size > static_cast<uint32_t>(kMaxBlockKb) * 1024)
    throw FormatError("BZZ block too large");

  std::vector<uint8_t> data(size);

  // Frequency adaptation speed, sent as a unary pair.
  int fshift = 0;
  if (zp.DecodePassThrough()) {
    fshift += 1;
    if (zp.DecodePassThrough()) fshift += 1;
  }

  // The move-to-front table starts as the identity permutation.
  uint8_t mtf[256];
  for (int i = 0; i < 256; ++i) mtf[i] = static_cast<uint8_t>(i);
  uint32_t freq[kFreqMax] = {};
  uint32_t fadd = 4;

  int mtfno = 3;
  int markerpos = -1;

  for (uint32_t i = 0; i < size; ++i) {
    // The rank is coded as a sequence of escapes: contexts are grouped so
    // that small ranks (the common case) cost a single bit.
    int ctxid = kCtxIds - 1;
    if (ctxid > mtfno) ctxid = mtfno;
    BitContext* cx = ctx;
    bool got = false;

    if (zp.Decode(cx[ctxid])) {
      mtfno = 0;
      got = true;
    } else {
      cx += kCtxIds;
      if (zp.Decode(cx[ctxid])) {
        mtfno = 1;
        got = true;
      } else {
        cx += kCtxIds;
        // Remaining ranks come as a power-of-two bucket plus an index
        // within it, each bucket having its own context tree.
        static const int kBase[] = {2, 4, 8, 16, 32, 64, 128};
        static const int kBits[] = {1, 2, 3, 4, 5, 6, 7};
        for (int b = 0; b < 7 && !got; ++b) {
          if (zp.Decode(cx[0])) {
            mtfno = kBase[b] +
                    static_cast<int>(zp.DecodeBinary(cx + 1, kBits[b]));
            got = true;
          } else {
            cx += 1 + (1 << kBits[b]) - 1;
          }
        }
      }
    }

    if (!got) {
      // Escape past rank 255 marks the position of the sentinel row of the
      // Burrows-Wheeler matrix.
      mtfno = 256;
      data[i] = 0;
      markerpos = static_cast<int>(i);
      continue;
    }

    data[i] = mtf[mtfno];

    // Re-sort the MTF table by running frequency rather than pure recency;
    // this is the "quasi-MTF" the format specifies.
    fadd = fadd + (fadd >> fshift);
    if (fadd > 0x10000000u) {
      fadd >>= 24;
      for (int k = 0; k < kFreqMax; ++k) freq[k] >>= 24;
    }
    uint32_t fc = fadd;
    if (mtfno < kFreqMax) fc += freq[mtfno];

    int k = mtfno;
    for (; k >= kFreqMax; --k) mtf[k] = mtf[k - 1];
    for (; k > 0 && fc >= freq[k - 1]; --k) {
      mtf[k] = mtf[k - 1];
      freq[k] = freq[k - 1];
    }
    mtf[k] = data[i];
    freq[k] = fc;
  }

  if (markerpos < 1 || static_cast<uint32_t>(markerpos) >= size)
    throw FormatError("BZZ block missing sentinel");

  // Invert the sort transform. posn[] packs each byte with its rank among
  // equal bytes, which is enough to walk the original order backwards.
  std::vector<uint32_t> posn(size, 0);
  int count[256] = {};
  for (int i = 0; i < markerpos; ++i) {
    const uint8_t c = data[i];
    posn[i] = (static_cast<uint32_t>(c) << 24) | (count[c] & 0xffffff);
    count[c] += 1;
  }
  for (uint32_t i = markerpos + 1; i < size; ++i) {
    const uint8_t c = data[i];
    posn[i] = (static_cast<uint32_t>(c) << 24) | (count[c] & 0xffffff);
    count[c] += 1;
  }

  int last = 1;
  for (int i = 0; i < 256; ++i) {
    const int tmp = count[i];
    count[i] = last;
    last += tmp;
  }

  const size_t base = out.size();
  out.resize(base + size - 1);
  int i = 0;
  int w = static_cast<int>(size) - 1;
  while (w > 0) {
    const uint32_t n = posn[i];
    const uint8_t c = static_cast<uint8_t>(posn[i] >> 24);
    out[base + static_cast<size_t>(--w)] = c;
    i = count[c] + static_cast<int>(n & 0xffffff);
  }
  if (i != markerpos) throw FormatError("BZZ inverse sort inconsistent");
  return true;
}

}  // namespace

std::vector<uint8_t> BzzDecompress(ByteSpan data) {
  ZpDecoder zp(data);
  BitContext ctx[kNumContexts] = {};
  std::vector<uint8_t> out;
  // A stream is a sequence of blocks terminated by a zero-length one. A
  // truncated stream throws from the ZP layer; we keep what we decoded.
  try {
    while (DecodeBlock(zp, ctx, out)) {
    }
  } catch (const FormatError&) {
    if (out.empty()) throw;
  }
  return out;
}

}  // namespace vellum::djvu
