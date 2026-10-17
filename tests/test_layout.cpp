// The layout engine: sheets, fit modes, rotation and hit testing.

#include "doctest.h"

#include <memory>

#include "fake_document.h"
#include "view/layout.h"

using namespace vellum;
using vellum::test::FakeDocument;

namespace {

constexpr int kGap = LayoutEngine::kPageGap;
constexpr int kMargin = LayoutEngine::kMargin;

}  // namespace

TEST_CASE("Layout defaults and setters clamp") {
  LayoutEngine e;
  CHECK(e.layout() == PageLayout::kContinuous);
  CHECK(e.zoom_mode() == ZoomMode::kFitWidth);
  CHECK(e.rotation() == 0);
  CHECK_FALSE(e.cover_page());
  CHECK(e.is_continuous());
  CHECK_FALSE(e.is_two_up());

  // Rotation normalises into 0..359 from either direction.
  e.set_rotation(-90);
  CHECK(e.rotation() == 270);
  e.set_rotation(450);
  CHECK(e.rotation() == 90);
  e.set_rotation(0);

  // Zoom is clamped to a usable range.
  e.set_zoom(1000.0);
  CHECK(e.zoom() < 100.0);
  e.set_zoom(0.0000001);
  CHECK(e.zoom() > 0.0);

  e.set_layout(PageLayout::kTwoPageContinuous);
  CHECK(e.is_continuous());
  CHECK(e.is_two_up());
  e.set_layout(PageLayout::kTwoPage);
  CHECK_FALSE(e.is_continuous());
  CHECK(e.is_two_up());
  e.set_layout(PageLayout::kSinglePage);
  CHECK_FALSE(e.is_continuous());
  CHECK_FALSE(e.is_two_up());
}

TEST_CASE("An engine with no document lays out nothing") {
  LayoutEngine e;
  const Layout l = e.Build(Size{800, 600}, 0);
  CHECK(l.pages.empty());
  CHECK(l.extent.w == 0);
  CHECK(e.Sheets().empty());

  FakeDocument empty(0);
  e.set_document(&empty);
  CHECK(e.Build(Size{800, 600}, 0).pages.empty());
}

TEST_CASE("Sheets group pages one-up and two-up") {
  FakeDocument doc(5);
  LayoutEngine e;
  e.set_document(&doc);

  e.set_layout(PageLayout::kContinuous);
  auto sheets = e.Sheets();
  REQUIRE(sheets.size() == 5);
  CHECK(sheets[0].first == 0);
  CHECK(sheets[0].second == -1);

  // Two-up without a cover pairs from the first page.
  e.set_layout(PageLayout::kTwoPageContinuous);
  sheets = e.Sheets();
  REQUIRE(sheets.size() == 3);
  CHECK(sheets[0] == std::make_pair(0, 1));
  CHECK(sheets[1] == std::make_pair(2, 3));
  CHECK(sheets[2] == std::make_pair(4, -1));   // odd page left alone

  // A cover page sits alone so the rest pair as a bound book does.
  e.set_cover_page(true);
  sheets = e.Sheets();
  REQUIRE(sheets.size() == 3);
  CHECK(sheets[0] == std::make_pair(0, -1));
  CHECK(sheets[1] == std::make_pair(1, 2));
  CHECK(sheets[2] == std::make_pair(3, 4));
}

TEST_CASE("Fit width makes a page exactly fill the viewport") {
  FakeDocument doc(3, 600, 800);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_zoom_mode(ZoomMode::kFitWidth);

  const Size viewport{824, 600};   // 600pt page + two margins
  const Layout l = e.Build(viewport, 0);
  REQUIRE_FALSE(l.pages.empty());
  CHECK(l.pages[0].rect.width() == viewport.w - 2 * kMargin);
  // The scrollable extent matches the viewport, so no horizontal scrolling.
  CHECK(l.extent.w == viewport.w);
}

TEST_CASE("Fit page fits whichever axis is tighter") {
  FakeDocument doc(1, 600, 800);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_zoom_mode(ZoomMode::kFitPage);

  // A viewport far wider than it is tall: height is the binding axis.
  const Layout l = e.Build(Size{4000, 424}, 0);
  REQUIRE_FALSE(l.pages.empty());
  CHECK(l.pages[0].rect.height() <= 424 - 2 * kMargin);
  CHECK(l.pages[0].rect.width() < 4000);

  e.set_zoom_mode(ZoomMode::kFitHeight);
  const Layout h = e.Build(Size{4000, 424}, 0);
  CHECK(h.pages[0].rect.height() == 424 - 2 * kMargin);
}

TEST_CASE("Actual size and custom zoom honour the display scale") {
  FakeDocument doc(1, 100, 100);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_dpi_scale(2.0);

  e.set_zoom_mode(ZoomMode::kActualSize);
  const Layout a = e.Build(Size{2000, 2000}, 0);
  // 100pt at 2x device scale is 200 device pixels.
  CHECK(a.pages[0].rect.width() == 200);
  CHECK(a.scale == doctest::Approx(2.0));

  e.set_zoom_mode(ZoomMode::kCustom);
  e.set_zoom(3.0);
  const Layout c = e.Build(Size{2000, 2000}, 0);
  CHECK(c.pages[0].rect.width() == 300);
}

TEST_CASE("Fit visible width behaves as fit width") {
  FakeDocument doc(1, 600, 800);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_zoom_mode(ZoomMode::kFitVisibleWidth);
  const Layout l = e.Build(Size{824, 600}, 0);
  CHECK(l.pages[0].rect.width() == 824 - 2 * kMargin);
}

TEST_CASE("Rotation swaps the page's axes") {
  FakeDocument doc(1, 600, 800);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_zoom_mode(ZoomMode::kCustom);
  e.set_zoom(1.0);

  const Layout up = e.Build(Size{2000, 2000}, 0);
  CHECK(up.pages[0].rect.width() == 600);
  CHECK(up.pages[0].rect.height() == 800);

  for (int deg : {90, 270}) {
    e.set_rotation(deg);
    const Layout side = e.Build(Size{2000, 2000}, 0);
    CHECK(side.pages[0].rect.width() == 800);
    CHECK(side.pages[0].rect.height() == 600);
    CHECK(side.rotation == deg);
  }

  e.set_rotation(180);
  const Layout flipped = e.Build(Size{2000, 2000}, 0);
  CHECK(flipped.pages[0].rect.width() == 600);
}

TEST_CASE("A paged mode lays out only the anchor's sheet") {
  FakeDocument doc(10);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_layout(PageLayout::kSinglePage);

  const Layout l = e.Build(Size{800, 600}, 4);
  REQUIRE(l.pages.size() == 1);
  CHECK(l.pages[0].page == 4);

  e.set_layout(PageLayout::kTwoPage);
  const Layout two = e.Build(Size{800, 600}, 4);
  REQUIRE(two.pages.size() == 2);
  CHECK(two.pages[0].page == 4);
  CHECK(two.pages[1].page == 5);

  // An out-of-range anchor is clamped rather than faulting.
  const Layout clamped = e.Build(Size{800, 600}, 999);
  CHECK_FALSE(clamped.pages.empty());
}

TEST_CASE("Continuous layout stacks every sheet with gaps") {
  FakeDocument doc(3, 100, 100);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_zoom_mode(ZoomMode::kCustom);
  e.set_zoom(1.0);

  const Layout l = e.Build(Size{500, 500}, 0);
  REQUIRE(l.pages.size() == 3);
  CHECK(l.pages[0].rect.y0 == kMargin);
  CHECK(l.pages[1].rect.y0 == kMargin + 100 + kGap);
  CHECK(l.pages[2].rect.y0 == kMargin + 2 * (100 + kGap));
  // The last sheet gets a margin, not a gap.
  CHECK(l.extent.h == kMargin + 3 * 100 + 2 * kGap + kMargin);
}

TEST_CASE("A two-up spread places its pages side by side") {
  FakeDocument doc(2, 100, 100);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_layout(PageLayout::kTwoPageContinuous);
  e.set_zoom_mode(ZoomMode::kCustom);
  e.set_zoom(1.0);

  const Layout l = e.Build(Size{600, 600}, 0);
  REQUIRE(l.pages.size() == 2);
  CHECK(l.pages[0].rect.y0 == l.pages[1].rect.y0);   // shared top edge
  CHECK(l.pages[1].rect.x0 == l.pages[0].rect.x1 + kGap);
}

TEST_CASE("A wider page does not shift the others off centre") {
  // The scrollable area grows to the widest page, but the narrow ones stay
  // centred in it rather than being pushed aside.
  FakeDocument doc(3, 100, 100);
  doc.SetPageSize(1, 400, 100);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_zoom_mode(ZoomMode::kCustom);
  e.set_zoom(1.0);

  const Layout l = e.Build(Size{200, 600}, 0);
  REQUIRE(l.pages.size() == 3);
  const int centre = l.extent.w / 2;
  for (const PagePlacement& p : l.pages) {
    const int page_centre = (p.rect.x0 + p.rect.x1) / 2;
    CHECK(std::abs(page_centre - centre) <= 1);
  }
}

TEST_CASE("Layout lookups: Find, Visible and DominantPage") {
  FakeDocument doc(4, 100, 100);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_zoom_mode(ZoomMode::kCustom);
  e.set_zoom(1.0);
  const Layout l = e.Build(Size{300, 300}, 0);

  REQUIRE(static_cast<bool>(l.Find(2)));
  CHECK(l.Find(2)->page == 2);
  CHECK_FALSE(static_cast<bool>(l.Find(99)));
  CHECK_FALSE(static_cast<bool>(l.Find(-1)));

  // A viewport over the first page sees it and nothing far below.
  const Rect top = Rect::FromXYWH(0, 0, 300, 50);
  const auto visible = l.Visible(top);
  CHECK(visible.size() >= 1);
  CHECK(visible[0]->page == 0);
  CHECK(l.DominantPage(top) == 0);

  // A viewport straddling two pages picks whichever fills more of it.
  const PagePlacement* p1 = l.Find(1);
  REQUIRE(static_cast<bool>(p1));
  const Rect mostly_second =
      Rect::FromXYWH(0, p1->rect.y0 - 5, 300, 100);
  CHECK(l.DominantPage(mostly_second) == 1);

  // Off the end of the document there is nothing to intersect, so the
  // first page stands in rather than returning nothing.
  const Rect far = Rect::FromXYWH(0, 100000, 10, 10);
  CHECK(l.Visible(far).empty());
  CHECK(l.DominantPage(far) == 0);

  CHECK(Layout().DominantPage(top) == -1);
}

TEST_CASE("EffectiveZoom reports what a fit mode resolves to") {
  FakeDocument doc(2, 600, 800);
  LayoutEngine e;
  e.set_document(&doc);

  // 824 of viewport less two margins is 800 device pixels for a 600pt
  // page, so the scale is 800/600 rather than 1.
  e.set_zoom_mode(ZoomMode::kFitWidth);
  CHECK(e.EffectiveZoom(Size{824, 600}, 0) ==
        doctest::Approx(800.0 / 600.0));

  // A viewport with no room falls back to the stored zoom instead of
  // producing a nonsensical scale.
  e.set_zoom(0.5);
  CHECK(e.EffectiveZoom(Size{1, 1}, 0) == doctest::Approx(0.5));
  CHECK(e.EffectiveZoom(Size{0, 0}, 0) == doctest::Approx(0.5));

  // Custom zoom is returned verbatim.
  e.set_zoom_mode(ZoomMode::kCustom);
  e.set_zoom(2.5);
  CHECK(e.EffectiveZoom(Size{824, 600}, 0) == doctest::Approx(2.5));
}

TEST_CASE("A two-up fit accounts for the whole spread") {
  FakeDocument doc(2, 300, 400);
  LayoutEngine e;
  e.set_document(&doc);
  e.set_layout(PageLayout::kTwoPageContinuous);
  e.set_zoom_mode(ZoomMode::kFitWidth);

  // Two 300pt pages plus the gap and margins must fit the viewport.
  const Layout l = e.Build(Size{900, 900}, 0);
  REQUIRE(l.pages.size() == 2);
  const int spread = l.pages[1].rect.x1 - l.pages[0].rect.x0;
  CHECK(spread <= 900 - 2 * kMargin);
}

TEST_CASE("A page reporting no size still gets a box") {
  // A backend that cannot measure a page must not collapse the layout.
  FakeDocument doc(2, 0, 0);
  LayoutEngine e;
  e.set_document(&doc);
  const Layout l = e.Build(Size{800, 600}, 0);
  REQUIRE_FALSE(l.pages.empty());
  CHECK(l.pages[0].rect.width() > 0);
  CHECK(l.pages[0].rect.height() > 0);
}
