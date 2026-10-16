#include "djvu/zp.h"

namespace vellum::djvu {

#include "djvu/zptable.inc"

ZpDecoder::ZpDecoder(ByteSpan data) : data_(data) {
  // The first two bytes preload the code register. A truncated stream reads
  // as 0xff; the delay counter below eventually turns that into an error.
  const uint8_t b0 = (next_ < data_.size()) ? data_.data()[next_++] : 0xff;
  const uint8_t b1 = (next_ < data_.size()) ? data_.data()[next_++] : 0xff;
  code_ = (static_cast<uint32_t>(b0) << 8) | b1;
  Preload();
  AdjustFence();
}

void ZpDecoder::Preload() {
  while (scount_ <= 24) {
    uint8_t byte;
    if (next_ < data_.size()) {
      byte = data_.data()[next_++];
    } else {
      byte = 0xff;
      if (--delay_ < 1)
        throw FormatError("ZP stream truncated");
    }
    buffer_ = (buffer_ << 8) | byte;
    scount_ += 8;
  }
}

int ZpDecoder::DecodeSub(BitContext& ctx, uint32_t z) {
  const int bit = ctx & 1;

  // Clamp the split point so the interval cannot invert.
  const uint32_t limit = 0x6000 + ((z + a_) >> 2);
  if (z > limit) z = limit;

  if (z > code_) {
    // Least-probable symbol: move into the complementary interval and take
    // the downward adaptation edge.
    z = 0x10000 - z;
    a_ += z;
    code_ += z;
    ctx = kZpTable[ctx].dn;
    Renorm(Ffz(a_));
    return bit ^ 1;
  }

  // Most-probable symbol: adapt upward only once the interval has shrunk
  // past this state's threshold.
  if (a_ >= kZpTable[ctx].m) ctx = kZpTable[ctx].up;
  a_ = z;
  Renorm(1);
  return bit;
}

int ZpDecoder::DecodeSubSimple(int mps, uint32_t z) {
  if (z > code_) {
    z = 0x10000 - z;
    a_ += z;
    code_ += z;
    Renorm(Ffz(a_));
    return mps ^ 1;
  }
  a_ = z;
  Renorm(1);
  return mps;
}

}  // namespace vellum::djvu
