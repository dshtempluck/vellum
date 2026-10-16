#include "pdf/document.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "core/bytes.h"
#include "fpdf_doc.h"
#include "fpdf_text.h"
#include "fpdfview.h"

namespace vellum::pdf {
namespace {

// PDFium wants a single process-wide init/teardown.
std::once_flag g_init_once;

void EnsureLibrary() {
  std::call_once(g_init_once, [] {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    config.m_pUserFontPaths = nullptr;
    config.m_pIsolate = nullptr;
    config.m_v8EmbedderSlot = 0;
    FPDF_InitLibraryWithConfig(&config);
    // Deliberately never destroyed: the viewer holds documents open for
    // its whole lifetime and teardown order at exit is not worth the risk.
  });
}

// Converts a UTF-16LE buffer as returned by PDFium into UTF-8.
std::string Utf16ToUtf8(const std::vector<unsigned short>& buf) {
  std::string out;
  out.reserve(buf.size());
  for (size_t i = 0; i < buf.size(); ++i) {
    unsigned int cp = buf[i];
    if (cp == 0) break;
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < buf.size() &&
        buf[i + 1] >= 0xdc00 && buf[i + 1] <= 0xdfff) {
      cp = 0x10000 + ((cp - 0xd800) << 10) + (buf[i + 1] - 0xdc00);
      ++i;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
  }
  return out;
}

std::string BookmarkTitle(FPDF_BOOKMARK bm) {
  const unsigned long bytes = FPDFBookmark_GetTitle(bm, nullptr, 0);
  if (bytes < 2) return std::string();
  std::vector<unsigned short> buf(bytes / 2 + 1, 0);
  FPDFBookmark_GetTitle(bm, buf.data(), bytes);
  return Utf16ToUtf8(buf);
}

std::string DocMetaText(FPDF_DOCUMENT doc, const char* tag) {
  const unsigned long bytes = FPDF_GetMetaText(doc, tag, nullptr, 0);
  if (bytes < 2) return std::string();
  std::vector<unsigned short> buf(bytes / 2 + 1, 0);
  FPDF_GetMetaText(doc, tag, buf.data(), bytes);
  return Utf16ToUtf8(buf);
}

const char* LoadErrorText(unsigned long err) {
  switch (err) {
    case FPDF_ERR_SUCCESS: return "no error";
    case FPDF_ERR_FILE: return "file not found or cannot be opened";
    case FPDF_ERR_FORMAT: return "not a valid PDF";
    case FPDF_ERR_PASSWORD: return "this PDF is password protected";
    case FPDF_ERR_SECURITY: return "unsupported PDF security scheme";
    case FPDF_ERR_PAGE: return "PDF page data is damaged";
    default: return "PDF could not be loaded";
  }
}

}  // namespace

std::unique_ptr<PdfDocument> PdfDocument::Open(std::vector<uint8_t> bytes) {
  EnsureLibrary();

  auto doc = std::unique_ptr<PdfDocument>(new PdfDocument());
  doc->bytes_ = std::move(bytes);

  // PDFium reads from the buffer lazily, so bytes_ must outlive doc_.
  FPDF_DOCUMENT handle = FPDF_LoadMemDocument64(
      doc->bytes_.data(), doc->bytes_.size(), nullptr);
  if (!handle) {
    const unsigned long err = FPDF_GetLastError();
    throw FormatError(LoadErrorText(err));
  }
  doc->doc_ = handle;
  doc->page_count_ = FPDF_GetPageCount(handle);
  if (doc->page_count_ <= 0) throw FormatError("PDF has no pages");

  doc->geometry_.resize(static_cast<size_t>(doc->page_count_));
  doc->LoadMetadata();
  doc->LoadOutline();
  return doc;
}

PdfDocument::~PdfDocument() {
  std::lock_guard<std::mutex> lock(mu_);
  if (doc_) FPDF_CloseDocument(static_cast<FPDF_DOCUMENT>(doc_));
  doc_ = nullptr;
}

void PdfDocument::LoadMetadata() {
  FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);
  title_ = DocMetaText(doc, "Title");

  int version = 0;
  if (FPDF_GetFileVersion(doc, &version) && version > 0) {
    // PDFium reports 17 for PDF 1.7, 20 for 2.0.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "PDF %d.%d", version / 10, version % 10);
    format_name_ = buf;
  }
}

void PdfDocument::LoadOutline() {
  FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);

  // Walk the bookmark tree. Depth is capped because a malformed file can
  // present a cycle, and PDFium does not detect one for us.
  struct Walker {
    FPDF_DOCUMENT doc;
    void Walk(FPDF_BOOKMARK parent, std::vector<OutlineItem>* into,
              int depth) {
      if (depth > 24) return;
      FPDF_BOOKMARK bm = FPDFBookmark_GetFirstChild(doc, parent);
      int guard = 0;
      while (bm && ++guard < 20000) {
        OutlineItem item;
        item.title = BookmarkTitle(bm);
        FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
        if (!dest) {
          // Many files express the destination through a GoTo action.
          FPDF_ACTION action = FPDFBookmark_GetAction(bm);
          if (action) dest = FPDFAction_GetDest(doc, action);
        }
        item.page = dest ? FPDFDest_GetDestPageIndex(doc, dest) : -1;
        item.open = depth == 0;
        Walk(bm, &item.children, depth + 1);
        into->push_back(std::move(item));
        bm = FPDFBookmark_GetNextSibling(doc, bm);
      }
    }
  };
  Walker w{doc};
  w.Walk(nullptr, &outline_, 0);
}

PageGeometry PdfDocument::geometry(int page) const {
  if (page < 0 || page >= page_count_) return PageGeometry();

  std::lock_guard<std::mutex> lock(mu_);
  PageGeometry& g = const_cast<PdfDocument*>(this)->geometry_[page];
  if (g.width_pt > 0) return g;

  FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);
  // GetPageSizeByIndexF avoids loading the whole page just to measure it.
  FS_SIZEF size{};
  if (FPDF_GetPageSizeByIndexF(doc, page, &size) && size.width > 0) {
    g.width_pt = size.width;
    g.height_pt = size.height;
  } else {
    g.width_pt = 612;
    g.height_pt = 792;
  }
  // PDFium's render path already applies /Rotate, so the viewer must not
  // apply it a second time.
  g.rotation = 0;
  return g;
}

RenderedPage PdfDocument::Render(int page, Size size, int rotation) {
  RenderedPage out;
  out.page = page;
  out.size = size;
  if (size.empty() || page < 0 || page >= page_count_) {
    out.error = "page out of range";
    return out;
  }

  out.pixmap.Init(size.w, size.h);

  std::lock_guard<std::mutex> lock(mu_);
  FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);
  FPDF_PAGE fpage = FPDF_LoadPage(doc, page);
  if (!fpage) {
    out.pixmap.Fill(0xff, 0xff, 0xff);
    out.error = "page could not be loaded";
    return out;
  }

  // Wrap our own buffer so PDFium rasterises straight into it. The
  // viewer's Pixmap is 24-bit BGR, which is both PDFium's FPDFBitmap_BGR
  // layout and what GDI blits without conversion.
  FPDF_BITMAP bmp = FPDFBitmap_CreateEx(
      size.w, size.h, FPDFBitmap_BGR, out.pixmap.row(0),
      static_cast<int>(out.pixmap.stride()));
  if (!bmp) {
    FPDF_ClosePage(fpage);
    out.pixmap.Fill(0xff, 0xff, 0xff);
    out.error = "could not allocate a render target";
    return out;
  }

  FPDFBitmap_FillRect(bmp, 0, 0, size.w, size.h, 0xffffffff);

  // rotation arrives in degrees; PDFium counts quarter turns clockwise.
  int quarters = ((rotation % 360) + 360) % 360 / 90;

  const int flags = FPDF_ANNOT |     // draw annotation appearances
                    FPDF_LCD_TEXT;   // subpixel-positioned text
  FPDF_RenderPageBitmap(bmp, fpage, 0, 0, size.w, size.h, quarters, flags);

  FPDFBitmap_Destroy(bmp);
  FPDF_ClosePage(fpage);

  out.complete = true;
  return out;
}

std::vector<TextRun> PdfDocument::Text(int page) {
  std::vector<TextRun> runs;
  if (page < 0 || page >= page_count_) return runs;

  std::lock_guard<std::mutex> lock(mu_);
  FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);
  FPDF_PAGE fpage = FPDF_LoadPage(doc, page);
  if (!fpage) return runs;
  FPDF_TEXTPAGE tp = FPDFText_LoadPage(fpage);
  if (!tp) {
    FPDF_ClosePage(fpage);
    return runs;
  }

  const double pw = FPDF_GetPageWidth(fpage);
  const double ph = FPDF_GetPageHeight(fpage);
  const int count = FPDFText_CountChars(tp);

  // Group characters into words. Word boundaries are what selection and
  // search want to snap to, and it keeps the run count manageable on
  // text-heavy pages.
  TextRun current;
  bool have = false;
  double cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;

  auto flush = [&] {
    if (!have || current.text.empty()) {
      have = false;
      current = TextRun();
      return;
    }
    if (pw > 0 && ph > 0) {
      current.box.x0 = static_cast<float>(cx0 / pw);
      current.box.x1 = static_cast<float>(cx1 / pw);
      // PDF user space has its origin at the bottom left.
      current.box.y0 = static_cast<float>(1.0 - cy1 / ph);
      current.box.y1 = static_cast<float>(1.0 - cy0 / ph);
      runs.push_back(current);
    }
    have = false;
    current = TextRun();
  };

  for (int i = 0; i < count; ++i) {
    const unsigned int ch = FPDFText_GetUnicode(tp, i);
    const bool is_space = ch == ' ' || ch == '\t' || ch == '\r' ||
                          ch == '\n' || ch == 0xa0 || ch == 0;
    if (is_space) {
      flush();
      continue;
    }

    double l = 0, r = 0, b = 0, t = 0;
    if (!FPDFText_GetCharBox(tp, i, &l, &r, &b, &t)) continue;

    if (!have) {
      current = TextRun();
      current.char_offset = i;
      cx0 = l;
      cx1 = r;
      cy0 = b;
      cy1 = t;
      have = true;
    } else {
      cx0 = std::min(cx0, l);
      cx1 = std::max(cx1, r);
      cy0 = std::min(cy0, b);
      cy1 = std::max(cy1, t);
    }

    // Append as UTF-8.
    if (ch < 0x80) {
      current.text.push_back(static_cast<char>(ch));
    } else if (ch < 0x800) {
      current.text.push_back(static_cast<char>(0xc0 | (ch >> 6)));
      current.text.push_back(static_cast<char>(0x80 | (ch & 0x3f)));
    } else {
      current.text.push_back(static_cast<char>(0xe0 | (ch >> 12)));
      current.text.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3f)));
      current.text.push_back(static_cast<char>(0x80 | (ch & 0x3f)));
    }
  }
  flush();

  FPDFText_ClosePage(tp);
  FPDF_ClosePage(fpage);
  return runs;
}

}  // namespace vellum::pdf
