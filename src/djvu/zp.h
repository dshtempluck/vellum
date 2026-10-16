// ZP coder: the adaptive binary arithmetic decoder underneath every DjVu
// codec (JB2, IW44, BZZ). Decode side only -- this is a viewer.
//
// The design is fixed by the DjVu bitstream: a 16-bit interval register
// `a`, a code register fed from a byte lookahead buffer, and a 256-state
// probability machine (zptable.inc) whose transitions carry both the
// probability estimate and the adaptation path.
#pragma once

#include <bit>
#include <cstdint>

#include "core/bytes.h"

namespace vellum::djvu {

// A context is an index into the state table. Its low bit holds the current
// most-probable symbol, so adaptation is a single table lookup.
using BitContext = uint8_t;

struct ZpState {
  uint16_t p;   // interval increment for the most-probable symbol
  uint16_t m;   // interval threshold gating upward adaptation
  uint8_t up;   // next state after a most-probable symbol
  uint8_t dn;   // next state after a least-probable symbol
};

extern const ZpState kZpTable[256];

class ZpDecoder {
 public:
  explicit ZpDecoder(ByteSpan data);

  // Decodes one bit using, and adapting, the given context.
  int Decode(BitContext& ctx) {
    const uint32_t z = a_ + kZpTable[ctx].p;
    if (z <= fence_) {  // overwhelmingly the common case
      a_ = z;
      return ctx & 1;
    }
    return DecodeSub(ctx, z);
  }

  // Pass-through decoder: bits written without compression.
  int DecodePassThrough() { return DecodeSubSimple(0, 0x8000 + (a_ >> 1)); }

  // The fixed-probability variant IW44 uses for coefficient bits.
  int DecodeIw() { return DecodeSubSimple(0, 0x8000 + ((a_ + a_ + a_) >> 3)); }

  // Reads `bits` bits MSB-first through the pass-through decoder.
  uint32_t DecodeRaw(int bits) {
    uint32_t n = 1;
    const uint32_t m = 1u << bits;
    while (n < m) n = (n << 1) | static_cast<uint32_t>(DecodePassThrough());
    return n - m;
  }

  // Reads `bits` bits using a run of contexts as a binary tree. The
  // bitstream addresses the tree one slot below its root, hence the -1.
  uint32_t DecodeBinary(BitContext* ctx, int bits) {
    uint32_t n = 1;
    const uint32_t m = 1u << bits;
    ctx -= 1;
    while (n < m) n = (n << 1) | static_cast<uint32_t>(Decode(ctx[n]));
    return n - m;
  }

 private:
  int DecodeSub(BitContext& ctx, uint32_t z);
  int DecodeSubSimple(int mps, uint32_t z);
  void Preload();
  void AdjustFence() { fence_ = (code_ >= 0x8000) ? 0x7fff : code_; }

  // Renormalization shift: the number of leading 1-bits of the 16-bit
  // interval register.
  static int Ffz(uint32_t x) {
    return std::countl_one(static_cast<uint16_t>(x));
  }

  void Renorm(int shift) {
    scount_ -= shift;
    a_ = static_cast<uint16_t>(a_ << shift);
    code_ = static_cast<uint16_t>(code_ << shift) |
            ((buffer_ >> scount_) & ((1u << shift) - 1));
    if (scount_ < 16) Preload();
    AdjustFence();
  }

  ByteSpan data_;
  size_t next_ = 0;      // next byte to feed the lookahead buffer
  int delay_ = 25;       // grace bytes past end-of-stream before giving up
  int scount_ = 0;       // valid bits held in buffer_
  uint32_t a_ = 0;       // interval register
  uint32_t code_ = 0;    // code register
  uint32_t fence_ = 0;   // fast-path threshold
  uint32_t buffer_ = 0;  // byte lookahead
};

}  // namespace vellum::djvu
