// Small geometry types used throughout the viewer.
#pragma once

#include <algorithm>

namespace vellum {

struct Point {
  int x = 0;
  int y = 0;
};

struct Size {
  int w = 0;
  int h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};

// Half-open integer rectangle: x0 <= x < x1, y0 <= y < y1.
struct Rect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

  static Rect FromXYWH(int x, int y, int w, int h) {
    return Rect{x, y, x + w, y + h};
  }

  int width() const { return x1 - x0; }
  int height() const { return y1 - y0; }
  bool empty() const { return x1 <= x0 || y1 <= y0; }

  bool Contains(int x, int y) const {
    return x >= x0 && x < x1 && y >= y0 && y < y1;
  }

  Rect Intersect(const Rect& o) const {
    return Rect{std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1),
                std::min(y1, o.y1)};
  }

  Rect Union(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return Rect{std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1),
                std::max(y1, o.y1)};
  }

  Rect Offset(int dx, int dy) const {
    return Rect{x0 + dx, y0 + dy, x1 + dx, y1 + dy};
  }

  Rect Inflate(int d) const { return Rect{x0 - d, y0 - d, x1 + d, y1 + d}; }

  bool Intersects(const Rect& o) const { return !Intersect(o).empty(); }
};

// Rectangle in a page's own coordinate space, normalised to 0..1 on both
// axes with the origin at the top left. Keeping selection and search
// results resolution-independent means they survive a zoom change.
struct RectF {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;

  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

}  // namespace vellum
