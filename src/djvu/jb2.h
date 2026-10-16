// JB2: DjVu's bitonal codec, and the chunk that carries most of the ink in
// a scanned document (Sjbz for a page mask, Djbz for a shared dictionary).
//
// JB2 is a symbol-matching coder. The page is described as a list of blits:
// small bitmaps ("shapes", in practice individual glyphs) placed at
// positions. Repeated glyphs cost only a library index and a position, and
// near-repeats are coded as refinements of an earlier shape. Pixels are
// coded with the ZP coder under a context formed from already-decoded
// neighbours.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/bitmap.h"
#include "core/bytes.h"
#include "djvu/zp.h"

namespace vellum::djvu {

struct Jb2Shape {
  Bitmap bits;
  int parent = -1;  // shape this one refines, -1 none, -2 non-mark data
};

struct Jb2Blit {
  int shapeno = 0;
  int left = 0;
  int bottom = 0;
};

// A shape dictionary: either standalone (decoded from a Djbz chunk) or the
// shapes accumulated while decoding a page.
class Jb2Dict {
 public:
  // Shapes are numbered across the inherited dictionary first, then local
  // ones, which is the numbering the bitstream uses.
  int shape_count() const {
    return inherited_count() + static_cast<int>(shapes_.size());
  }
  int inherited_count() const {
    return inherited_ ? inherited_->shape_count() : 0;
  }

  const Jb2Shape& shape(int i) const;
  Jb2Shape& shape(int i);

  int AddShape(Jb2Shape s) {
    shapes_.push_back(std::move(s));
    return inherited_count() + static_cast<int>(shapes_.size()) - 1;
  }

  void set_inherited(std::shared_ptr<const Jb2Dict> d) {
    inherited_ = std::move(d);
  }
  const std::shared_ptr<const Jb2Dict>& inherited() const {
    return inherited_;
  }

  std::string comment;

 private:
  std::vector<Jb2Shape> shapes_;
  std::shared_ptr<const Jb2Dict> inherited_;
};

// A decoded JB2 page: the shapes plus where they are stamped.
class Jb2Image : public Jb2Dict {
 public:
  int width = 0;
  int height = 0;
  std::vector<Jb2Blit> blits;

  // Composes the blits into a mask. `subsample` >= 1 reduces resolution,
  // producing coverage counts instead of a bilevel result.
  Bitmap Compose(int subsample = 1) const;
};

// Decodes a Djbz dictionary chunk, optionally on top of an inherited one.
std::shared_ptr<Jb2Dict> DecodeJb2Dict(
    ByteSpan data, std::shared_ptr<const Jb2Dict> inherited);

// Decodes an Sjbz page-mask chunk.
std::shared_ptr<Jb2Image> DecodeJb2Image(
    ByteSpan data, std::shared_ptr<const Jb2Dict> inherited);

}  // namespace vellum::djvu
