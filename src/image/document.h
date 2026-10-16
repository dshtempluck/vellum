// Still-image backend: JPEG, PNG and whatever else the platform decodes.
//
// Opening an image opens its *folder*. Every image beside it becomes a
// page, sorted the way Explorer sorts them, and the file that was asked
// for becomes the initial page. That is what makes the existing
// next/previous controls step through the pictures in a directory, and it
// comes with thumbnails, the page box, zoom, rotation and continuous
// scroll for free, because none of those know what a page is made of.
//
// Decoding goes through the Windows Imaging Component, which ships with
// the OS: JPEG, PNG, BMP, GIF and TIFF everywhere, plus WebP and HEIF on
// current Windows. WIC also scales and rotates, so a downscaled page is
// resampled properly rather than point-sampled.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "doc/document.h"

namespace vellum::image {

// True if WIC is likely to decode this file, judged by extension. Used to
// decide what to collect from a folder, not to decide what will open.
bool HasImageExtension(const std::wstring& path);

// True if these leading bytes are a recognised image signature.
bool LooksLikeImage(const uint8_t* data, size_t size);

class ImageDocument : public Document {
 public:
  // `path` is one image; its folder supplies the rest of the pages.
  static std::unique_ptr<ImageDocument> Open(const std::wstring& path);

  int page_count() const override { return static_cast<int>(files_.size()); }
  PageGeometry geometry(int page) const override;
  RenderedPage Render(int page, Size size, int rotation) override;
  std::vector<TextRun> Text(int page) override { (void)page; return {}; }
  const std::vector<OutlineItem>& outline() const override { return none_; }
  std::string format_name() const override;
  std::string title() const override;
  int initial_page() const override { return initial_; }
  bool pages_are_independent() const override { return true; }
  std::string page_label(int page) const override;

 private:
  ImageDocument() = default;

  // Reads just enough of a file to learn its size, orientation and
  // resolution. Cheap compared with decoding, which matters because the
  // layout engine measures every page in the folder.
  void LoadGeometry(int page) const;

  std::wstring folder_;
  std::vector<std::wstring> files_;  // file names, not full paths
  int initial_ = 0;

  // Per-page geometry, filled on demand. `exif_` holds the EXIF
  // orientation code (1..8); it has to be applied to both the reported
  // size and the rendered pixels or a phone photo comes out sideways.
  mutable std::vector<PageGeometry> geometry_;
  mutable std::vector<int> exif_;
  mutable std::vector<bool> measured_;
  mutable std::mutex mu_;

  std::vector<OutlineItem> none_;
};

}  // namespace vellum::image
