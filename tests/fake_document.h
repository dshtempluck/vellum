// A Document implementation under the test's control.
//
// The layout engine and the render cache are written against the Document
// interface and nothing else, so they can be exercised without decoding
// anything. This also lets a test ask for things a real file would make
// awkward: pages of differing size, a page that fails to render, or a
// render slow enough to observe the cache working.
#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/bytes.h"
#include "doc/document.h"

namespace vellum::test {

class FakeDocument : public Document {
 public:
  explicit FakeDocument(int pages = 3, double w = 612, double h = 792)
      : pages_(pages) {
    for (int i = 0; i < pages; ++i) geometry_.push_back(PageGeometry{w, h, 0});
  }

  // Replaces one page's size, for mixed-size layout tests.
  void SetPageSize(int page, double w, double h, int rotation = 0) {
    if (page >= 0 && page < static_cast<int>(geometry_.size()))
      geometry_[page] = PageGeometry{w, h, rotation};
  }

  int page_count() const override { return pages_; }

  PageGeometry geometry(int page) const override {
    if (page < 0 || page >= static_cast<int>(geometry_.size()))
      return PageGeometry();
    return geometry_[page];
  }

  RenderedPage Render(int page, Size size, int rotation) override {
    ++renders;
    if (render_delay_ms > 0)
      std::this_thread::sleep_for(
          std::chrono::milliseconds(render_delay_ms.load()));
    RenderedPage out;
    out.page = page;
    out.size = size;
    if (fail_render) {
      out.error = "fake failure";
      return out;  // deliberately leaves the pixmap empty
    }
    if (throw_on_render) throw FormatError("fake throw");
    out.pixmap.Init(size.w, size.h);
    // Encode the page number in the first pixel so a test can tell which
    // rendering it got back.
    if (!out.pixmap.empty()) {
      out.pixmap.row(0)[0] = static_cast<uint8_t>(page & 0xff);
      out.pixmap.row(0)[1] = static_cast<uint8_t>(rotation / 90);
    }
    out.complete = true;
    return out;
  }

  std::vector<TextRun> Text(int page) override {
    std::vector<TextRun> runs;
    if (page < 0 || page >= pages_) return runs;
    // Two words per page, laid out on one line.
    for (int i = 0; i < 2; ++i) {
      TextRun r;
      r.text = (i == 0) ? "hello" : "world";
      r.char_offset = i * 6;
      r.box.x0 = 0.1f + 0.4f * i;
      r.box.x1 = r.box.x0 + 0.3f;
      r.box.y0 = 0.1f;
      r.box.y1 = 0.15f;
      runs.push_back(std::move(r));
    }
    return runs;
  }

  const std::vector<OutlineItem>& outline() const override { return outline_; }
  std::string format_name() const override { return "Fake"; }
  std::string title() const override { return title_; }
  int initial_page() const override { return initial_page_; }
  bool pages_are_independent() const override { return independent_; }
  std::string page_label(int page) const override {
    return labelled_ ? ("page" + std::to_string(page)) : std::string();
  }

  // Knobs the tests reach for.
  std::vector<OutlineItem> outline_;
  std::string title_;
  int initial_page_ = 0;
  bool independent_ = false;
  bool labelled_ = false;
  bool fail_render = false;
  bool throw_on_render = false;
  std::atomic<int> render_delay_ms{0};
  std::atomic<int> renders{0};

 private:
  int pages_;
  std::vector<PageGeometry> geometry_;
};

}  // namespace vellum::test
