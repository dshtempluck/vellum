// IW44: DjVu's wavelet codec for continuous-tone layers -- the background
// (BG44) and foreground colour (FG44) of a page, and standalone wavelet
// images (BM44/PM44).
//
// An image is split into 32x32 blocks of wavelet coefficients, grouped into
// 10 frequency bands of 16-coefficient buckets. Coefficients are sent
// bit-plane by bit-plane, so a chunk sequence refines progressively: each
// chunk carries a number of "slices", and a viewer that stops early still
// has a complete, lower-quality image.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/bitmap.h"
#include "core/bytes.h"
#include "djvu/zp.h"

namespace vellum::djvu {

class Iw44Decoder {
 public:
  Iw44Decoder();
  ~Iw44Decoder();

  // Feeds one BG44/FG44/BM44/PM44 chunk. Chunks must arrive in order; each
  // one refines the image decoded so far.
  void AddChunk(ByteSpan chunk);

  bool ready() const { return y_ != nullptr; }
  int width() const { return width_; }
  int height() const { return height_; }
  bool is_color() const { return cb_ != nullptr; }

  // Reconstructs the image. Returns an empty pixmap if nothing decoded.
  Pixmap ToPixmap() const;

 private:
  // One colour plane's coefficients.
  struct Map;
  // Per-plane bit-plane decoding state.
  struct Codec;

  std::unique_ptr<Map> y_, cb_, cr_;
  std::unique_ptr<Codec> ycodec_, cbcodec_, crcodec_;

  int width_ = 0;
  int height_ = 0;
  int serial_ = 0;      // chunks consumed so far
  int slice_ = 0;       // slices consumed so far
  int crcb_delay_ = 0;  // slices before chroma starts; -1 means grayscale
  bool crcb_half_ = false;  // chroma stored at half resolution
};

}  // namespace vellum::djvu
