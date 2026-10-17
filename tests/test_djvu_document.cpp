// The DjVu document layer: directory, pages, text and outline.
//
// These run against the fetched corpus, which is the only practical source
// of real JB2 and IW44 streams.

#include "doctest.h"

#include <functional>
#include <set>
#include <string>

#include "core/bytes.h"
#include "djvu/document.h"
#include "fixtures.h"

using namespace vellum;
using namespace vellum::djvu;

namespace {

std::unique_ptr<DjvuDocument> OpenSample(const wchar_t* name) {
  return DjvuDocument::Open(test::ReadFile(test::Sample(name)));
}

}  // namespace

TEST_CASE("A bundled multi-page document exposes its pages") {
  if (!test::HasCorpus()) return;
  auto doc = OpenSample(L"djvu3spec.djvu");
  REQUIRE(static_cast<bool>(doc));

  CHECK(doc->page_count() == 71);
  CHECK(doc->format_name() == "DjVu (multi-page)");

  const PageGeometry g = doc->geometry(0);
  // 8.5 x 11 inches, as a US Letter scan.
  CHECK(g.width_pt == doctest::Approx(612.0).epsilon(0.01));
  CHECK(g.height_pt == doctest::Approx(792.0).epsilon(0.01));
}

TEST_CASE("Every page of every corpus document measures") {
  if (!test::HasCorpus()) return;
  for (const wchar_t* name :
       {L"djvu2spec.djvu", L"djvu3spec.djvu", L"djvulibre-book-en.djvu",
        L"djvulibre-book-ru.djvu", L"lizard2002.djvu", L"lizard2007.djvu"}) {
    auto doc = OpenSample(name);
    REQUIRE(static_cast<bool>(doc));
    CHECK(doc->page_count() > 0);
    for (int p = 0; p < doc->page_count(); ++p) {
      const PageGeometry g = doc->geometry(p);
      CHECK(g.width_pt > 0);
      CHECK(g.height_pt > 0);
    }
  }
}

TEST_CASE("The outline is a tree with resolvable destinations") {
  if (!test::HasCorpus()) return;
  auto doc = OpenSample(L"djvu3spec.djvu");
  REQUIRE(static_cast<bool>(doc));

  const auto& outline = doc->outline();
  REQUIRE_FALSE(outline.empty());

  // Walk it: titles are present, pages are either unresolved or in range,
  // and the structure is genuinely nested.
  int nodes = 0;
  int with_children = 0;
  std::function<void(const std::vector<OutlineItem>&, int)> walk =
      [&](const std::vector<OutlineItem>& items, int depth) {
        REQUIRE(depth < 16);
        for (const OutlineItem& it : items) {
          ++nodes;
          CHECK_FALSE(it.title.empty());
          CHECK(it.page >= -1);
          CHECK(it.page < doc->page_count());
          if (!it.children.empty()) ++with_children;
          walk(it.children, depth + 1);
        }
      };
  walk(outline, 0);
  CHECK(nodes > 10);
  CHECK(with_children > 0);
}

TEST_CASE("A document with no outline reports none") {
  if (!test::HasCorpus()) return;
  auto doc = OpenSample(L"lizard2002.djvu");
  REQUIRE(static_cast<bool>(doc));
  CHECK(doc->outline().empty());
}

TEST_CASE("The hidden text layer yields words with page-relative boxes") {
  if (!test::HasCorpus()) return;
  auto doc = OpenSample(L"djvu3spec.djvu");
  REQUIRE(static_cast<bool>(doc));

  const auto runs = doc->Text(0);
  REQUIRE(runs.size() > 100);

  std::string joined;
  for (const TextRun& r : runs) {
    CHECK_FALSE(r.text.empty());
    CHECK(r.box.x0 >= -0.01f);
    CHECK(r.box.x1 <= 1.01f);
    CHECK(r.box.y0 >= -0.01f);
    CHECK(r.box.y1 <= 1.01f);
    CHECK(r.box.x1 > r.box.x0);
    CHECK(r.box.y1 > r.box.y0);
    CHECK(r.char_offset >= 0);
    joined += r.text + " ";
  }
  // The first page of the DjVu specification.
  CHECK(joined.find("Lizardtech") != std::string::npos);
  CHECK(joined.find("Introduction") != std::string::npos);

  // Text is cached, so asking again gives the same answer.
  CHECK(doc->Text(0).size() == runs.size());
  // Out of range is empty rather than an error.
  CHECK(doc->Text(-1).empty());
  CHECK(doc->Text(9999).empty());
}

TEST_CASE("A bitonal page renders and is mostly white with some ink") {
  if (!test::HasCorpus()) return;
  auto doc = OpenSample(L"djvu3spec.djvu");
  REQUIRE(static_cast<bool>(doc));

  RenderedPage out = doc->Render(0, Size{300, 388}, 0);
  REQUIRE(out.complete);
  REQUIRE(out.pixmap.width() == 300);
  REQUIRE(out.pixmap.height() == 388);

  long long dark = 0, light = 0;
  for (int y = 0; y < out.pixmap.height(); ++y) {
    const uint8_t* row = out.pixmap.row(y);
    for (int x = 0; x < out.pixmap.width(); ++x) {
      if (row[x * 3] < 100) ++dark;
      else if (row[x * 3] > 200) ++light;
    }
  }
  // A page of text: predominantly paper, with a real amount of ink.
  CHECK(light > dark * 3);
  CHECK(dark > 200);
}

TEST_CASE("A colour page carries colour") {
  if (!test::HasCorpus()) return;
  auto doc = OpenSample(L"lizard2002.djvu");
  REQUIRE(static_cast<bool>(doc));

  RenderedPage out = doc->Render(0, Size{400, 518}, 0);
  REQUIRE(out.complete);

  // The LizardTech letterhead means at least some pixels are not grey.
  int coloured = 0;
  for (int y = 0; y < out.pixmap.height(); ++y) {
    const uint8_t* row = out.pixmap.row(y);
    for (int x = 0; x < out.pixmap.width(); ++x) {
      const int b = row[x * 3], g = row[x * 3 + 1], r = row[x * 3 + 2];
      const int spread = std::max({b, g, r}) - std::min({b, g, r});
      if (spread > 40) ++coloured;
    }
  }
  CHECK(coloured > 100);
}

TEST_CASE("Rendering is stable across sizes and rotations") {
  if (!test::HasCorpus()) return;
  auto doc = OpenSample(L"lizard2007.djvu");
  REQUIRE(static_cast<bool>(doc));

  for (int deg : {0, 90, 180, 270}) {
    const bool swap = (deg % 180) == 90;
    const Size size = swap ? Size{200, 155} : Size{155, 200};
    RenderedPage out = doc->Render(0, size, deg);
    CHECK(out.complete);
    CHECK(out.pixmap.width() == size.w);
    CHECK(out.pixmap.height() == size.h);
  }

  // A very small target still produces a usable bitmap rather than
  // dividing by zero somewhere in the subsampling.
  RenderedPage tiny = doc->Render(0, Size{1, 1}, 0);
  CHECK(tiny.pixmap.width() == 1);
}

TEST_CASE("Pages outside the document are refused") {
  if (!test::HasCorpus()) return;
  auto doc = OpenSample(L"lizard2002.djvu");
  REQUIRE(static_cast<bool>(doc));

  CHECK_FALSE(doc->Render(-1, Size{50, 50}, 0).complete);
  CHECK_FALSE(doc->Render(999, Size{50, 50}, 0).complete);
  CHECK_FALSE(doc->Render(0, Size{0, 0}, 0).complete);
  // A page that does not exist measures as nothing, the same as every
  // other backend. The letter-size fallback is for a page that exists but
  // carries no usable INFO, which is a different case.
  CHECK(doc->geometry(999).width_pt == 0);
  CHECK(doc->geometry(-1).width_pt == 0);
}

TEST_CASE("Shared shape dictionaries are reused across pages") {
  if (!test::HasCorpus()) return;
  // Pages of this document INCL a common Djbz dictionary; rendering
  // several in a row exercises the cache that keeps it decoded once.
  auto doc = OpenSample(L"djvulibre-book-en.djvu");
  REQUIRE(static_cast<bool>(doc));
  for (int p = 0; p < 5; ++p) {
    RenderedPage out = doc->Render(p, Size{200, 260}, 0);
    CHECK(out.complete);
  }
}

TEST_CASE("An indirect document is reported, not silently emptied") {
  // A DJVM whose directory is not bundled holds only an index; the pages
  // live in separate files this viewer does not follow.
  std::vector<uint8_t> dirm;
  dirm.push_back(0x01);          // version 1, bundled bit clear
  test::PutU16(&dirm, 1);        // one component
  dirm.resize(dirm.size() + 8, 0);

  std::vector<uint8_t> body = test::MakeChunk("DIRM", dirm);
  const std::vector<uint8_t> file = test::MakeForm("DJVM", body, true);
  // Either the directory fails to parse or the indirect case is reported;
  // both are errors rather than an empty document.
  CHECK_THROWS_AS(DjvuDocument::Open(file), FormatError);
}

TEST_CASE("A container that is not DjVu at all is refused") {
  const std::vector<uint8_t> wrong =
      test::MakeForm("AIFF", test::MakeChunk("COMM", {1, 2}), true);
  CHECK_THROWS_AS(DjvuDocument::Open(wrong), FormatError);
}
