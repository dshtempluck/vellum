// The still-image backend: signatures, folder enumeration, ordering.

#include "doctest.h"

#include <windows.h>

#include <string>

#include "core/bytes.h"
#include "fixtures.h"
#include "image/document.h"

using namespace vellum;
using namespace vellum::image;

TEST_CASE("Extensions are recognised case-insensitively") {
  CHECK(HasImageExtension(L"a.jpg"));
  CHECK(HasImageExtension(L"a.JPG"));
  CHECK(HasImageExtension(L"a.JpEg"));
  CHECK(HasImageExtension(L"C:\\x\\y.png"));
  CHECK(HasImageExtension(L"a.bmp"));
  CHECK(HasImageExtension(L"a.webp"));
  CHECK(HasImageExtension(L"a.heic"));
  CHECK(HasImageExtension(L"a.tiff"));

  CHECK_FALSE(HasImageExtension(L"a.pdf"));
  CHECK_FALSE(HasImageExtension(L"a.djvu"));
  CHECK_FALSE(HasImageExtension(L"a.txt"));
  CHECK_FALSE(HasImageExtension(L"noextension"));
  CHECK_FALSE(HasImageExtension(L""));
  // A dot in a directory name must not be mistaken for an extension.
  CHECK_FALSE(HasImageExtension(L"C:\\dir.png\\file"));
}

TEST_CASE("Signatures identify the common formats") {
  auto looks = [](std::vector<uint8_t> v) {
    v.resize(32, 0);
    return LooksLikeImage(v.data(), v.size());
  };

  CHECK(looks({0xff, 0xd8, 0xff, 0xe0}));                       // JPEG
  CHECK(looks({0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}));  // PNG
  CHECK(looks({'B', 'M'}));                                     // BMP
  CHECK(looks({'G', 'I', 'F', '8', '7', 'a'}));
  CHECK(looks({'G', 'I', 'F', '8', '9', 'a'}));
  CHECK(looks({'I', 'I', 0x2a, 0x00}));                         // TIFF LE
  CHECK(looks({'M', 'M', 0x00, 0x2a}));                         // TIFF BE
  CHECK(looks({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'}));
  CHECK(looks({0, 0, 0, 0, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'}));
  CHECK(looks({0, 0, 0, 0, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'}));

  CHECK_FALSE(looks({'%', 'P', 'D', 'F', '-'}));
  CHECK_FALSE(looks({'A', 'T', '&', 'T'}));
  CHECK_FALSE(looks({0, 0, 0, 0, 'f', 't', 'y', 'p', 'q', 't', ' ', ' '}));

  // Too short to carry any signature at all.
  const uint8_t two[] = {'B', 'M'};
  CHECK_FALSE(LooksLikeImage(two, 2));
}

TEST_CASE("An unreadable image is reported rather than opened") {
  test::TempDir dir;
  // Correct BMP signature, nothing decodable behind it.
  std::vector<uint8_t> bad = {'B', 'M'};
  bad.resize(128, 0);
  const std::wstring p = dir.Write(L"broken.bmp", bad);
  CHECK_THROWS_AS(ImageDocument::Open(p), FormatError);
}

TEST_CASE("A lone image still presents as a one-page document") {
  test::TempDir dir;
  const std::wstring p =
      dir.Write(L"only.bmp", test::MakeBmp(40, 30, 10, 20, 30));
  auto doc = ImageDocument::Open(p);
  REQUIRE(static_cast<bool>(doc));

  CHECK(doc->page_count() == 1);
  CHECK(doc->initial_page() == 0);
  CHECK(doc->pages_are_independent());
  CHECK(doc->format_name() == "Bitmap image");
  CHECK(doc->page_label(0) == "only.bmp");
  CHECK(doc->outline().empty());
  CHECK(doc->Text(0).empty());
  CHECK_FALSE(doc->title().empty());   // the folder's name

  // 40x30 pixels at the 72 dpi the fixture records.
  // The fixture records 2835 pixels per metre, which is 72.009 dpi, so
  // the point size lands a hair under a whole number.
  const PageGeometry g = doc->geometry(0);
  CHECK(g.width_pt == doctest::Approx(40.0).epsilon(0.001));
  CHECK(g.height_pt == doctest::Approx(30.0).epsilon(0.001));
  CHECK(g.rotation == 0);

  // Out-of-range pages answer safely.
  CHECK(doc->page_label(-1).empty());
  CHECK(doc->page_label(99).empty());
  CHECK(doc->geometry(99).width_pt == 0);
}

TEST_CASE("A folder becomes the document, sorted the way Explorer sorts") {
  test::TempDir dir;
  // Deliberately created out of order, and with names that a plain
  // lexicographic sort would get wrong.
  dir.Write(L"pic10.bmp", test::MakeBmp(10, 10, 255, 0, 0));
  dir.Write(L"pic2.bmp", test::MakeBmp(20, 10, 0, 255, 0));
  dir.Write(L"pic1.bmp", test::MakeBmp(30, 10, 0, 0, 255));
  dir.Write(L"notes.txt", {'h', 'i'});          // not an image
  const std::wstring second = dir.path() + L"\\pic2.bmp";

  auto doc = ImageDocument::Open(second);
  REQUIRE(static_cast<bool>(doc));

  CHECK(doc->page_count() == 3);                // the text file is excluded
  CHECK(doc->page_label(0) == "pic1.bmp");
  CHECK(doc->page_label(1) == "pic2.bmp");
  CHECK(doc->page_label(2) == "pic10.bmp");     // natural order, not "10 < 2"

  // The file that was asked for is the one shown first.
  CHECK(doc->initial_page() == 1);
  CHECK(doc->format_name() == "Images (3 in folder)");

  // Each page reports its own size.
  CHECK(doc->geometry(0).width_pt == doctest::Approx(30.0).epsilon(0.001));
  CHECK(doc->geometry(1).width_pt == doctest::Approx(20.0).epsilon(0.001));
  CHECK(doc->geometry(2).width_pt == doctest::Approx(10.0).epsilon(0.001));
}

TEST_CASE("Images render at the requested size with the right colours") {
  test::TempDir dir;
  const std::wstring p =
      dir.Write(L"red.bmp", test::MakeBmp(16, 16, 255, 0, 0));
  auto doc = ImageDocument::Open(p);
  REQUIRE(static_cast<bool>(doc));

  RenderedPage out = doc->Render(0, Size{8, 8}, 0);
  CHECK(out.complete);
  CHECK(out.error.empty());
  REQUIRE(out.pixmap.width() == 8);
  REQUIRE(out.pixmap.height() == 8);
  // Pixmap is BGR, so pure red is (0, 0, 255).
  CHECK(out.pixmap.row(0)[0] == 0);
  CHECK(out.pixmap.row(0)[1] == 0);
  CHECK(out.pixmap.row(0)[2] == 255);

  // Upscaling works as well as downscaling.
  RenderedPage big = doc->Render(0, Size{64, 64}, 0);
  CHECK(big.complete);
  CHECK(big.pixmap.width() == 64);
}

TEST_CASE("Rotation swaps the rendered axes") {
  test::TempDir dir;
  const std::wstring p =
      dir.Write(L"wide.bmp", test::MakeBmp(40, 10, 0, 255, 0));
  auto doc = ImageDocument::Open(p);
  REQUIRE(static_cast<bool>(doc));

  for (int deg : {90, 180, 270}) {
    RenderedPage out = doc->Render(0, Size{10, 40}, deg);
    CHECK(out.complete);
    CHECK(out.pixmap.width() == 10);
    CHECK(out.pixmap.height() == 40);
  }
}

TEST_CASE("Rendering outside the page range is refused safely") {
  test::TempDir dir;
  const std::wstring p = dir.Write(L"x.bmp", test::MakeBmp(8, 8, 1, 1, 1));
  auto doc = ImageDocument::Open(p);
  REQUIRE(static_cast<bool>(doc));

  CHECK_FALSE(doc->Render(-1, Size{8, 8}, 0).complete);
  CHECK_FALSE(doc->Render(99, Size{8, 8}, 0).complete);
  CHECK_FALSE(doc->Render(0, Size{0, 0}, 0).complete);
  CHECK_FALSE(doc->Render(0, Size{8, 8}, 0).error.size() > 0);
}

TEST_CASE("A file that disappears between listing and rendering is handled") {
  test::TempDir dir;
  dir.Write(L"a.bmp", test::MakeBmp(8, 8, 1, 1, 1));
  const std::wstring b = dir.Write(L"b.bmp", test::MakeBmp(8, 8, 2, 2, 2));
  auto doc = ImageDocument::Open(b);
  REQUIRE(static_cast<bool>(doc));
  REQUIRE(doc->page_count() == 2);

  DeleteFileW((dir.path() + L"\\a.bmp").c_str());
  RenderedPage out = doc->Render(0, Size{8, 8}, 0);
  CHECK_FALSE(out.complete);
  CHECK_FALSE(out.error.empty());
  // The surviving page still works.
  CHECK(doc->Render(1, Size{8, 8}, 0).complete);
}

TEST_CASE("EXIF orientation is applied to the reported size") {
  test::TempDir dir;
  // Orientations 5 to 8 are the quarter turns, which swap the axes; the
  // rest leave them alone. Getting this wrong leaves a phone photo
  // sideways, so it is checked for every code the tag defines.
  for (int orientation = 1; orientation <= 8; ++orientation) {
    wchar_t name[32]{};
    swprintf(name, 32, L"o%d.tif", orientation);
    const std::wstring p =
        dir.Write(name, test::MakeTiff(40, 20, orientation));
    auto doc = ImageDocument::Open(p);
    REQUIRE(static_cast<bool>(doc));

    // Opening an image opens its folder, and earlier iterations left
    // their files behind, so the page under test is the one the backend
    // opens on rather than the first in sort order.
    const PageGeometry g = doc->geometry(doc->initial_page());
    const bool swaps = orientation >= 5;
    CAPTURE(orientation);
    if (swaps) {
      CHECK(g.width_pt < g.height_pt);
    } else {
      CHECK(g.width_pt > g.height_pt);
    }
    // The orientation is baked into the reported size rather than left for
    // the viewer to apply a second time.
    CHECK(g.rotation == 0);
  }
}

TEST_CASE("EXIF orientation is applied to the pixels too") {
  test::TempDir dir;
  for (int orientation = 1; orientation <= 8; ++orientation) {
    wchar_t name[32]{};
    swprintf(name, 32, L"p%d.tif", orientation);
    const std::wstring p =
        dir.Write(name, test::MakeTiff(32, 16, orientation));
    auto doc = ImageDocument::Open(p);
    REQUIRE(static_cast<bool>(doc));

    const bool swaps = orientation >= 5;
    const Size size = swaps ? Size{16, 32} : Size{32, 16};
    RenderedPage out = doc->Render(doc->initial_page(), size, 0);
    CAPTURE(orientation);
    CHECK(out.complete);
    CHECK(out.pixmap.width() == size.w);
    CHECK(out.pixmap.height() == size.h);
  }
}

TEST_CASE("An orientation combines with the viewer's own rotation") {
  test::TempDir dir;
  // Orientation 6 is a quarter turn; a further 90 degrees of view rotation
  // puts the picture back on its original axis.
  const std::wstring p = dir.Write(L"r.tif", test::MakeTiff(40, 20, 6));
  auto doc = ImageDocument::Open(p);
  REQUIRE(static_cast<bool>(doc));

  RenderedPage upright = doc->Render(doc->initial_page(), Size{20, 40}, 0);
  CHECK(upright.complete);
  RenderedPage turned = doc->Render(doc->initial_page(), Size{40, 20}, 90);
  CHECK(turned.complete);
  CHECK(turned.pixmap.width() == 40);
}

TEST_CASE("A TIFF without an orientation tag is treated as upright") {
  test::TempDir dir;
  // Orientation 1 is the default; the reported size is the stored size.
  const std::wstring p = dir.Write(L"plain.tif", test::MakeTiff(50, 25, 1));
  auto doc = ImageDocument::Open(p);
  REQUIRE(static_cast<bool>(doc));
  const PageGeometry g = doc->geometry(doc->initial_page());
  CHECK(g.width_pt > g.height_pt);
  CHECK(doc->format_name() == "TIFF image");
}
