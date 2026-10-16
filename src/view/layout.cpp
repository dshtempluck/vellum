#include "view/layout.h"

#include <algorithm>
#include <cmath>

namespace vellum {
namespace {

// Zoom limits, matching what a viewer can usefully show.
constexpr double kMinZoom = 0.08;
constexpr double kMaxZoom = 64.0;

}  // namespace

const PagePlacement* Layout::Find(int page) const {
  for (const PagePlacement& p : pages)
    if (p.page == page) return &p;
  return nullptr;
}

std::vector<const PagePlacement*> Layout::Visible(const Rect& view) const {
  std::vector<const PagePlacement*> out;
  for (const PagePlacement& p : pages)
    if (p.rect.Intersects(view)) out.push_back(&p);
  return out;
}

int Layout::DominantPage(const Rect& view) const {
  int best = -1;
  long long best_area = 0;
  for (const PagePlacement& p : pages) {
    const Rect hit = p.rect.Intersect(view);
    if (hit.empty()) continue;
    const long long area =
        static_cast<long long>(hit.width()) * hit.height();
    // Ties go to the earlier page, which keeps the page box stable while
    // scrolling across a boundary.
    if (area > best_area) {
      best_area = area;
      best = p.page;
    }
  }
  if (best < 0 && !pages.empty()) best = pages.front().page;
  return best;
}

void LayoutEngine::set_zoom(double z) {
  zoom_ = std::clamp(z, kMinZoom, kMaxZoom);
}

void LayoutEngine::set_rotation(int degrees) {
  rotation_ = ((degrees % 360) + 360) % 360;
}

Size LayoutEngine::PagePixels(int page, double scale) const {
  if (!doc_) return Size{0, 0};
  const PageGeometry g = doc_->geometry(page);
  double w = g.width_pt;
  double h = g.height_pt;
  if (w <= 0 || h <= 0) {
    w = 612;
    h = 792;
  }
  // A quarter-turn view rotation swaps the axes. The document's own
  // rotation is handled by the backend, which reports geometry already
  // oriented.
  if (rotation_ == 90 || rotation_ == 270) std::swap(w, h);
  Size s;
  s.w = std::max(1, static_cast<int>(std::lround(w * scale)));
  s.h = std::max(1, static_cast<int>(std::lround(h * scale)));
  return s;
}

std::vector<std::pair<int, int>> LayoutEngine::Sheets() const {
  std::vector<std::pair<int, int>> sheets;
  if (!doc_) return sheets;
  const int n = doc_->page_count();

  if (!is_two_up()) {
    sheets.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) sheets.emplace_back(i, -1);
    return sheets;
  }

  int i = 0;
  // With a cover page the first sheet holds page 1 alone, so that the
  // remaining spreads pair even with odd the way a bound book does.
  if (cover_page_ && n > 0) {
    sheets.emplace_back(0, -1);
    i = 1;
  }
  for (; i < n; i += 2)
    sheets.emplace_back(i, (i + 1 < n) ? i + 1 : -1);
  return sheets;
}

double LayoutEngine::EffectiveZoom(Size viewport, int page) const {
  if (!doc_ || viewport.empty()) return zoom_;

  if (zoom_mode_ == ZoomMode::kCustom) return zoom_;
  if (zoom_mode_ == ZoomMode::kActualSize) return dpi_scale_;

  // Fit modes measure against the sheet the anchor page sits on, so a
  // two-up spread fits as a spread rather than as a single page.
  const PageGeometry g = doc_->geometry(page);
  double pw = g.width_pt > 0 ? g.width_pt : 612;
  double ph = g.height_pt > 0 ? g.height_pt : 792;
  if (rotation_ == 90 || rotation_ == 270) std::swap(pw, ph);

  if (is_two_up()) {
    // Width of the widest spread the page could appear in.
    int partner = -1;
    for (const auto& s : Sheets()) {
      if (s.first == page) {
        partner = s.second;
        break;
      }
      if (s.second == page) {
        partner = s.first;
        break;
      }
    }
    if (partner >= 0) {
      const PageGeometry pg = doc_->geometry(partner);
      double w2 = pg.width_pt > 0 ? pg.width_pt : 612;
      double h2 = pg.height_pt > 0 ? pg.height_pt : 792;
      if (rotation_ == 90 || rotation_ == 270) std::swap(w2, h2);
      pw += w2;
      ph = std::max(ph, h2);
    }
  }

  const double avail_w =
      viewport.w - 2.0 * kMargin - (is_two_up() ? kPageGap : 0);
  const double avail_h = viewport.h - 2.0 * kMargin;
  if (avail_w <= 1 || avail_h <= 1) return zoom_;

  double z = zoom_;
  switch (zoom_mode_) {
    case ZoomMode::kFitWidth:
    case ZoomMode::kFitVisibleWidth:
      z = avail_w / pw;
      break;
    case ZoomMode::kFitHeight:
      z = avail_h / ph;
      break;
    case ZoomMode::kFitPage:
      z = std::min(avail_w / pw, avail_h / ph);
      break;
    default:
      break;
  }
  return std::clamp(z, kMinZoom, kMaxZoom);
}

Layout LayoutEngine::Build(Size viewport, int anchor_page) const {
  Layout out;
  out.rotation = rotation_;
  if (!doc_ || doc_->page_count() <= 0) return out;

  const int n = doc_->page_count();
  anchor_page = std::clamp(anchor_page, 0, n - 1);

  const double scale = EffectiveZoom(viewport, anchor_page);
  out.scale = scale;

  std::vector<std::pair<int, int>> sheets = Sheets();

  if (!is_continuous()) {
    // Only the anchor's sheet exists in the scrollable area.
    std::pair<int, int> only{anchor_page, -1};
    for (const auto& s : sheets) {
      if (s.first == anchor_page || s.second == anchor_page) {
        only = s;
        break;
      }
    }
    sheets.assign(1, only);
  }

  // Measure every sheet first so the document can be centred on the
  // widest one rather than drifting left and right while scrolling.
  int widest = 0;
  std::vector<Size> sheet_size(sheets.size());
  for (size_t i = 0; i < sheets.size(); ++i) {
    const Size a = PagePixels(sheets[i].first, scale);
    Size s = a;
    if (sheets[i].second >= 0) {
      const Size b = PagePixels(sheets[i].second, scale);
      s.w = a.w + kPageGap + b.w;
      s.h = std::max(a.h, b.h);
    }
    sheet_size[i] = s;
    widest = std::max(widest, s.w);
  }

  // Centre sheets in whichever is wider, the viewport or the widest sheet.
  // Without the viewport term, a single landscape page in an otherwise
  // portrait document would widen the scrollable area and push every other
  // page off to one side.
  const int content_w =
      std::max(widest, std::max(0, viewport.w - 2 * kMargin));
  const int total_w = content_w + 2 * kMargin;

  int y = kMargin;
  for (size_t i = 0; i < sheets.size(); ++i) {
    const Size s = sheet_size[i];
    const int x0 = kMargin + (content_w - s.w) / 2;

    const Size a = PagePixels(sheets[i].first, scale);
    PagePlacement pa;
    pa.page = sheets[i].first;
    // Pages of differing heights in one spread sit on a shared top edge.
    pa.rect = Rect::FromXYWH(x0, y, a.w, a.h);
    out.pages.push_back(pa);

    if (sheets[i].second >= 0) {
      const Size b = PagePixels(sheets[i].second, scale);
      PagePlacement pb;
      pb.page = sheets[i].second;
      pb.rect = Rect::FromXYWH(x0 + a.w + kPageGap, y, b.w, b.h);
      out.pages.push_back(pb);
    }

    y += s.h + kPageGap;
  }
  // The last sheet needs a bottom margin, not a gap.
  y = y - kPageGap + kMargin;

  out.extent = Size{total_w, y};
  return out;
}

}  // namespace vellum
