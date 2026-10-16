// Raster types shared by the DjVu codecs and the renderer.
//
// Bitmap follows the convention the DjVu codecs are defined against: one
// byte per pixel, rows stored bottom-up (row 0 is the bottom scanline), and
// a border of zero pixels around the image so that context windows may read
// a few pixels outside it without bounds checks.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vellum {

class Bitmap {
 public:
  Bitmap() = default;
  Bitmap(int rows, int cols, int border = 0) { Init(rows, cols, border); }

  void Init(int rows, int cols, int border = 0);

  int rows() const { return rows_; }
  int cols() const { return cols_; }
  int border() const { return border_; }
  bool empty() const { return rows_ == 0 || cols_ == 0; }

  // Largest pixel value in use. JB2 masks are bilevel (1); subsampled or
  // anti-aliased masks carry higher maxima.
  int grays() const { return grays_; }
  void set_grays(int g) { grays_ = g; }

  // Pointer to column 0 of row `y`. Valid for y in
  // [-border, rows + border), and the returned row may be indexed from
  // -border to cols + border - 1.
  uint8_t* row(int y) {
    return data_.data() + (static_cast<size_t>(y + border_) * stride_) +
           border_;
  }
  const uint8_t* row(int y) const {
    return data_.data() + (static_cast<size_t>(y + border_) * stride_) +
           border_;
  }

  size_t stride() const { return stride_; }

  // Grows the border to at least `want`, preserving contents. The JB2 cross
  // coder needs a border that depends on the shape being refined, so this
  // can be called after the pixels are in place.
  void MinBorder(int want);

  void Fill(uint8_t v);

  // ORs `src` into this bitmap with its bottom-left corner at (x, y).
  // Used to compose JB2 shape blits into the page mask.
  void Blit(const Bitmap& src, int x, int y);

  // ORs `src` into this bitmap scaled down by `subsample`, accumulating
  // coverage counts rather than a boolean OR, which is how DjVu produces an
  // anti-aliased mask at reduced resolution.
  void BlitSubsampled(const Bitmap& src, int x, int y, int subsample);

 private:
  int rows_ = 0;
  int cols_ = 0;
  int border_ = 0;
  int grays_ = 2;
  size_t stride_ = 0;
  std::vector<uint8_t> data_;
};

// 24-bit colour raster, top-down, channels in B, G, R order.
//
// That order is deliberate: it is what a Windows DIB section expects and
// what PDFium's FPDFBitmap_BGR produces, so a rendered page reaches the
// screen with no per-pixel conversion anywhere in the pipeline.
//
// Rows are padded to a 4-byte boundary for the same reason. A DIB's
// stride is always rounded up to 4 bytes, and GDI derives it from the
// width alone -- so a tightly packed 3-byte-per-pixel buffer whose width
// is not a multiple of 4 is read back skewed, each row shifted a little
// further than the last.
class Pixmap {
 public:
  Pixmap() = default;
  Pixmap(int w, int h) { Init(w, h); }

  void Init(int w, int h) {
    w_ = w;
    h_ = h;
    stride_ = (static_cast<size_t>(w) * 3 + 3) & ~size_t{3};
    data_.assign(stride_ * static_cast<size_t>(h), 0xff);
  }

  int width() const { return w_; }
  int height() const { return h_; }
  bool empty() const { return w_ == 0 || h_ == 0; }

  uint8_t* row(int y) { return data_.data() + static_cast<size_t>(y) * stride_; }
  const uint8_t* row(int y) const {
    return data_.data() + static_cast<size_t>(y) * stride_;
  }

  // Bytes per row, including the padding to a 4-byte boundary.
  size_t stride() const { return stride_; }

  void Fill(uint8_t b, uint8_t g, uint8_t r);

 private:
  int w_ = 0;
  int h_ = 0;
  size_t stride_ = 0;
  std::vector<uint8_t> data_;
};

}  // namespace vellum
