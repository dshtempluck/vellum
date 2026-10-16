// The document abstraction the viewer is written against.
//
// Acrobat separates its PDF object model (Cos), its document semantics
// (PD) and its viewer (AV), so that the viewer never touches file
// structure. This interface plays the PD role here: the view and UI layers
// know only about pages, outlines, text and rendered bitmaps, which is what
// lets one viewer serve both PDF and DjVu.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/bitmap.h"
#include "core/geometry.h"

namespace vellum {

struct PageGeometry {
  // Page size in points (1/72 inch), before view rotation.
  double width_pt = 0;
  double height_pt = 0;
  // Intrinsic rotation declared by the document, in degrees clockwise.
  int rotation = 0;
};

// One run of text with its position on the page, in normalised page
// coordinates. Used for selection, copy and search highlighting.
struct TextRun {
  std::string text;  // UTF-8
  RectF box;
  // Index of the first character of this run within the page's text.
  int char_offset = 0;
};

struct OutlineItem {
  std::string title;
  int page = -1;       // destination page, -1 if it resolves to nothing
  bool open = false;   // whether the node starts expanded
  std::vector<OutlineItem> children;
};

// A rendered page bitmap: 24-bit BGR, top-down, ready to blit.
struct RenderedPage {
  Pixmap pixmap;
  int page = -1;
  Size size;
  bool complete = false;  // false if the backend could only partly decode
  std::string error;      // human-readable reason when the page failed
};

class Document {
 public:
  virtual ~Document() = default;

  virtual int page_count() const = 0;
  virtual PageGeometry geometry(int page) const = 0;

  // Renders `page` at exactly `size` pixels. Implementations must be safe
  // to call from a worker thread.
  virtual RenderedPage Render(int page, Size size, int rotation) = 0;

  // Page text, in reading order, with per-run boxes. Empty when the
  // document carries no text layer.
  virtual std::vector<TextRun> Text(int page) = 0;

  virtual const std::vector<OutlineItem>& outline() const = 0;

  // A short description of the format, for the title bar and properties.
  virtual std::string format_name() const = 0;

  // Document-level metadata, empty when absent.
  virtual std::string title() const { return std::string(); }
};

// Opens `path`, choosing a backend by content and extension. Throws
// FormatError if nothing can read it.
std::unique_ptr<Document> OpenDocument(const std::wstring& path);

}  // namespace vellum
