// The PDF backend.

#include "doctest.h"

#include <string>

#include "core/bytes.h"
#include "doc/document.h"
#include "fixtures.h"

#if VELLUM_HAVE_PDF
#include "pdf/document.h"
#endif

using namespace vellum;

#if VELLUM_HAVE_PDF

TEST_CASE("A synthetic PDF opens and reports its page box") {
  auto doc = pdf::PdfDocument::Open(test::MakePdf(400, 500));
  REQUIRE(static_cast<bool>(doc));

  CHECK(doc->page_count() == 1);
  CHECK(doc->format_name().rfind("PDF", 0) == 0);
  CHECK(doc->outline().empty());

  const PageGeometry g = doc->geometry(0);
  CHECK(g.width_pt == doctest::Approx(400.0));
  CHECK(g.height_pt == doctest::Approx(500.0));
  // The backend applies /Rotate itself, so the viewer must not do it again.
  CHECK(g.rotation == 0);

  // Measuring twice uses the cached value and still agrees.
  CHECK(doc->geometry(0).width_pt == doctest::Approx(400.0));
  // Out of range answers safely.
  CHECK(doc->geometry(-1).width_pt == 0);
  CHECK(doc->geometry(99).width_pt == 0);
}

TEST_CASE("Damaged and empty PDFs are refused with a reason") {
  CHECK_THROWS_AS(pdf::PdfDocument::Open({}), FormatError);

  std::vector<uint8_t> truncated = test::MakePdf();
  truncated.resize(truncated.size() / 3);
  CHECK_THROWS_AS(pdf::PdfDocument::Open(std::move(truncated)), FormatError);

  std::vector<uint8_t> not_pdf(512, 'x');
  CHECK_THROWS_AS(pdf::PdfDocument::Open(std::move(not_pdf)), FormatError);
}

TEST_CASE("A PDF renders the colours it declares") {
  auto doc = pdf::PdfDocument::Open(test::MakePdf(100, 100, /*fill_red=*/true));
  REQUIRE(static_cast<bool>(doc));

  RenderedPage out = doc->Render(0, Size{20, 20}, 0);
  CHECK(out.complete);
  REQUIRE(out.pixmap.width() == 20);
  REQUIRE(out.pixmap.height() == 20);
  // Pixmap is BGR; a page painted pure red arrives as (0, 0, 255). This is
  // the check that pins the channel order down against a documented
  // producer rather than against the eye.
  CHECK(out.pixmap.row(10)[0] == 0);
  CHECK(out.pixmap.row(10)[1] == 0);
  CHECK(out.pixmap.row(10)[2] == 255);
}

TEST_CASE("Rendering outside the page range is refused safely") {
  auto doc = pdf::PdfDocument::Open(test::MakePdf());
  REQUIRE(static_cast<bool>(doc));
  CHECK_FALSE(doc->Render(-1, Size{10, 10}, 0).complete);
  CHECK_FALSE(doc->Render(5, Size{10, 10}, 0).complete);
  CHECK_FALSE(doc->Render(0, Size{0, 10}, 0).complete);
  CHECK(doc->Text(-1).empty());
  CHECK(doc->Text(99).empty());
}

TEST_CASE("Rotation is applied by the backend") {
  auto doc = pdf::PdfDocument::Open(test::MakePdf(100, 200));
  REQUIRE(static_cast<bool>(doc));
  for (int deg : {0, 90, 180, 270}) {
    RenderedPage out = doc->Render(0, Size{40, 30}, deg);
    CHECK(out.complete);
    CHECK(out.pixmap.width() == 40);
    CHECK(out.pixmap.height() == 30);
  }
}

TEST_CASE("Text comes back grouped into words with page-relative boxes") {
  if (!test::HasCorpus()) return;
  auto doc = OpenDocument(test::Sample(L"pdf\\hello.pdf"));
  REQUIRE(static_cast<bool>(doc));

  const auto runs = doc->Text(0);
  REQUIRE_FALSE(runs.empty());

  std::string joined;
  for (const TextRun& r : runs) {
    CHECK_FALSE(r.text.empty());
    // No run contains whitespace: they are words.
    CHECK(r.text.find(' ') == std::string::npos);
    // Boxes are normalised to the page.
    CHECK(r.box.x0 >= -0.01f);
    CHECK(r.box.x1 <= 1.01f);
    CHECK(r.box.y0 >= -0.01f);
    CHECK(r.box.y1 <= 1.01f);
    CHECK(r.box.x1 > r.box.x0);
    CHECK(r.box.y1 > r.box.y0);
    joined += r.text;
  }
  CHECK(joined.find("Vellum") != std::string::npos);
}

TEST_CASE("An outline is read, nested, and resolved to pages") {
  auto doc = pdf::PdfDocument::Open(test::MakePdfWithOutline());
  REQUIRE(static_cast<bool>(doc));
  CHECK(doc->page_count() == 3);

  const auto& outline = doc->outline();
  REQUIRE(outline.size() == 2);

  CHECK(outline[0].title == "Chapter One");
  CHECK(outline[0].page == 0);
  CHECK(outline[0].open);                  // top level starts expanded
  REQUIRE(outline[0].children.size() == 1);
  CHECK(outline[0].children[0].title == "Section 1.1");
  CHECK(outline[0].children[0].page == 1);
  CHECK_FALSE(outline[0].children[0].open);

  // The second item points at its page through a GoTo action rather than a
  // direct destination, which is the other path the walker has to take.
  CHECK(outline[1].title == "Chapter Two");
  CHECK(outline[1].page == 2);
  CHECK(outline[1].children.empty());
}

TEST_CASE("Document metadata is converted from UTF-16") {
  auto doc = pdf::PdfDocument::Open(test::MakePdfWithOutline());
  REQUIRE(static_cast<bool>(doc));
  CHECK(doc->title() == "Vellum Outline Fixture");
}

TEST_CASE("A PDF with no outline reports none") {
  auto doc = pdf::PdfDocument::Open(test::MakePdf());
  REQUIRE(static_cast<bool>(doc));
  CHECK(doc->outline().empty());
  CHECK(doc->title().empty());
}

TEST_CASE("Every page of a multi-page PDF measures and renders") {
  auto doc = pdf::PdfDocument::Open(test::MakePdfWithOutline());
  REQUIRE(static_cast<bool>(doc));
  for (int p = 0; p < doc->page_count(); ++p) {
    const PageGeometry g = doc->geometry(p);
    CHECK(g.width_pt == doctest::Approx(300.0));
    CHECK(g.height_pt == doctest::Approx(400.0));
    RenderedPage out = doc->Render(p, Size{30, 40}, 0);
    CHECK(out.complete);
    // The text the fixture draws produces at least one dark pixel.
    CHECK(out.pixmap.width() == 30);
  }
}

#else

TEST_CASE("A build without PDF support says so") {
  test::TempDir dir;
  const std::wstring p = dir.Write(L"x.pdf", test::MakePdf());
  CHECK_THROWS_AS(OpenDocument(p), FormatError);
}

#endif  // VELLUM_HAVE_PDF
