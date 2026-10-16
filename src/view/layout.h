// Page layout: turning a document plus a zoom setting into page rectangles.
//
// This is the piece Acrobat calls the page-view distributor. It owns the
// six layout modes PDF defines (single page, continuous, and two-up in
// each case, with or without a cover page) and the fit modes the toolbar
// exposes. Everything downstream -- scrolling, hit testing, thumbnails,
// printing -- reads the rectangles it produces, so there is exactly one
// place where page geometry is decided.
#pragma once

#include <vector>

#include "core/geometry.h"
#include "doc/document.h"

namespace vellum {

enum class PageLayout {
  kSinglePage,          // one page at a time
  kContinuous,          // one column, scrolls between pages
  kTwoPage,             // spreads, one at a time
  kTwoPageContinuous,   // spreads, scrolling
};

enum class ZoomMode {
  kCustom,          // zoom() is used verbatim
  kFitPage,         // whole page visible
  kFitWidth,        // page width fills the viewport
  kFitVisibleWidth, // content width fills the viewport, ignoring margins
  kFitHeight,       // page height fills the viewport
  kActualSize,      // 100%, using the document's own dimensions
};

struct PagePlacement {
  int page = 0;
  Rect rect;  // position in document space, in device pixels
};

// A laid-out document: page rectangles plus the total extent to scroll.
struct Layout {
  Size extent;                       // full scrollable size
  std::vector<PagePlacement> pages;  // in page order
  double scale = 1.0;                // device pixels per point
  int rotation = 0;                  // view rotation in degrees

  // The placement for `page`, or null when the page is not laid out.
  const PagePlacement* Find(int page) const;

  // Pages whose rectangles intersect `view`, which is in document space.
  std::vector<const PagePlacement*> Visible(const Rect& view) const;

  // The page occupying most of `view`: what the page-number box shows and
  // what "current page" means for navigation.
  int DominantPage(const Rect& view) const;
};

// Computes layout for a document.
//
// `viewport` is the area available for pages, already excluding
// scrollbars; `dpi_scale` is the display scale (96 dpi = 1.0) so that
// 100% zoom means physically correct size.
class LayoutEngine {
 public:
  void set_document(Document* doc) { doc_ = doc; }

  void set_layout(PageLayout l) { layout_ = l; }
  PageLayout layout() const { return layout_; }

  void set_cover_page(bool v) { cover_page_ = v; }
  bool cover_page() const { return cover_page_; }

  void set_zoom_mode(ZoomMode m) { zoom_mode_ = m; }
  ZoomMode zoom_mode() const { return zoom_mode_; }

  // Zoom as a ratio: 1.0 is 100%.
  void set_zoom(double z);
  double zoom() const { return zoom_; }

  void set_rotation(int degrees);
  int rotation() const { return rotation_; }

  void set_dpi_scale(double s) { dpi_scale_ = s; }

  // Gap between pages and around the document, in device pixels.
  static constexpr int kPageGap = 12;
  static constexpr int kMargin = 12;

  // Builds the layout for the given viewport. In a one-page-at-a-time
  // mode only `anchor_page`'s sheet is laid out, because that is all the
  // view can scroll to.
  Layout Build(Size viewport, int anchor_page) const;

  // The zoom a fit mode resolves to for `page` in `viewport`. Exposed so
  // the toolbar can show the effective percentage.
  double EffectiveZoom(Size viewport, int page) const;

  // True when the mode lays out every page in one scrollable run.
  bool is_continuous() const {
    return layout_ == PageLayout::kContinuous ||
           layout_ == PageLayout::kTwoPageContinuous;
  }
  bool is_two_up() const {
    return layout_ == PageLayout::kTwoPage ||
           layout_ == PageLayout::kTwoPageContinuous;
  }

  // Groups pages into sheets: one page per sheet, or two side by side.
  // With a cover page, page 0 sits alone on the first sheet.
  std::vector<std::pair<int, int>> Sheets() const;

 private:
  // Page size in device pixels at the current scale and rotation.
  Size PagePixels(int page, double scale) const;

  Document* doc_ = nullptr;
  PageLayout layout_ = PageLayout::kContinuous;
  ZoomMode zoom_mode_ = ZoomMode::kFitWidth;
  double zoom_ = 1.0;
  double dpi_scale_ = 1.0;
  int rotation_ = 0;
  bool cover_page_ = false;
};

}  // namespace vellum
