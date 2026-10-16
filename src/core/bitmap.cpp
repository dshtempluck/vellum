#include "core/bitmap.h"

namespace vellum {

void Bitmap::Init(int rows, int cols, int border) {
  rows_ = std::max(0, rows);
  cols_ = std::max(0, cols);
  border_ = std::max(0, border);
  stride_ = static_cast<size_t>(cols_) + 2 * border_;
  data_.assign(stride_ * (static_cast<size_t>(rows_) + 2 * border_), 0);
  grays_ = 2;
}

void Bitmap::MinBorder(int want) {
  if (want <= border_) return;
  Bitmap grown(rows_, cols_, want);
  grown.grays_ = grays_;
  for (int y = 0; y < rows_; ++y)
    std::memcpy(grown.row(y), row(y), static_cast<size_t>(cols_));
  *this = std::move(grown);
}

void Bitmap::Fill(uint8_t v) {
  for (int y = 0; y < rows_; ++y)
    std::memset(row(y), v, static_cast<size_t>(cols_));
}

void Bitmap::Blit(const Bitmap& src, int x, int y) {
  const int y0 = std::max(0, -y);
  const int y1 = std::min(src.rows(), rows_ - y);
  const int x0 = std::max(0, -x);
  const int x1 = std::min(src.cols(), cols_ - x);
  for (int sy = y0; sy < y1; ++sy) {
    const uint8_t* s = src.row(sy);
    uint8_t* d = row(sy + y);
    for (int sx = x0; sx < x1; ++sx)
      if (s[sx]) d[sx + x] = 1;
  }
}

void Bitmap::BlitSubsampled(const Bitmap& src, int x, int y, int subsample) {
  if (subsample <= 1) {
    Blit(src, x, y);
    return;
  }
  for (int sy = 0; sy < src.rows(); ++sy) {
    const uint8_t* s = src.row(sy);
    const int dy = (sy + y) / subsample;
    // Negative coordinates floor-divide the wrong way; skip them.
    if ((sy + y) < 0 || dy < 0 || dy >= rows_) continue;
    uint8_t* d = row(dy);
    for (int sx = 0; sx < src.cols(); ++sx) {
      if (!s[sx]) continue;
      const int px = sx + x;
      if (px < 0) continue;
      const int dx = px / subsample;
      if (dx >= cols_) continue;
      if (d[dx] < 0xff) ++d[dx];
    }
  }
}

void Pixmap::Fill(uint8_t b, uint8_t g, uint8_t r) {
  for (int y = 0; y < h_; ++y) {
    uint8_t* p = row(y);
    for (int x = 0; x < w_; ++x) {
      p[x * 3 + 0] = b;
      p[x * 3 + 1] = g;
      p[x * 3 + 2] = r;
    }
  }
}

}  // namespace vellum
