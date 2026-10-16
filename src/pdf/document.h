// PDF document backend.
//
// Acrobat splits this work across AdobePDFL (object model and page
// rendering), AGM (rasterizer) and CoolType (fonts). PDFium occupies the
// same position here: it owns parsing, the graphics pipeline and font
// handling, and this file adapts it to the viewer's Document interface.
//
// PDFium's document and page objects are not safe to touch from more than
// one thread at a time, so every call into it is serialised here. The
// viewer's render worker is a single thread, which keeps that cheap.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "doc/document.h"

namespace vellum::pdf {

class PdfDocument : public Document {
 public:
  static std::unique_ptr<PdfDocument> Open(std::vector<uint8_t> bytes);
  ~PdfDocument() override;

  int page_count() const override { return page_count_; }
  PageGeometry geometry(int page) const override;
  RenderedPage Render(int page, Size size, int rotation) override;
  std::vector<TextRun> Text(int page) override;
  const std::vector<OutlineItem>& outline() const override { return outline_; }
  std::string format_name() const override { return format_name_; }
  std::string title() const override { return title_; }

 private:
  PdfDocument() = default;

  void LoadOutline();
  void LoadMetadata();

  std::vector<uint8_t> bytes_;  // PDFium reads this in place, so it must live
  void* doc_ = nullptr;         // FPDF_DOCUMENT
  int page_count_ = 0;
  std::string format_name_ = "PDF";
  std::string title_;
  std::vector<OutlineItem> outline_;
  std::vector<PageGeometry> geometry_;
  mutable std::mutex mu_;
};

}  // namespace vellum::pdf
