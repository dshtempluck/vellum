// DjVu document backend.
//
// A DjVu file is an IFF container. A single page is a FORM:DJVU; a
// multi-page document is a FORM:DJVM whose DIRM chunk indexes component
// files -- pages (FORM:DJVU), shared shape dictionaries (FORM:DJVI) and
// thumbnails (FORM:THUM). A page references the dictionaries it needs by
// INCL chunks.
//
// Each page is up to three layers: a bitonal mask (Sjbz/Smmr) holding text
// and line art at full resolution, a continuous-tone background (BG44) at
// reduced resolution, and a foreground colour layer (FG44, or a palette in
// FGbz). The renderer composes them: ink from the mask takes the
// foreground colour, everything else shows the background.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/bytes.h"
#include "djvu/iff.h"
#include "djvu/jb2.h"
#include "doc/document.h"

namespace vellum::djvu {

class DjvuDocument : public Document {
 public:
  // Takes ownership of the file bytes; chunk bodies are views into them.
  static std::unique_ptr<DjvuDocument> Open(std::vector<uint8_t> bytes);

  int page_count() const override;
  PageGeometry geometry(int page) const override;
  RenderedPage Render(int page, Size size, int rotation) override;
  std::vector<TextRun> Text(int page) override;
  const std::vector<OutlineItem>& outline() const override { return outline_; }
  std::string format_name() const override;
  std::string title() const override { return title_; }

 private:
  // One entry of the document directory.
  struct Component {
    std::string id;
    std::string name;
    std::string title;
    int type = 0;  // 0 include, 1 page, 2 thumbnails, 3 shared annotations
    size_t offset = 0;
    size_t size = 0;
    Chunk form;  // resolved lazily
    bool resolved = false;
  };

  // Everything needed to draw one page, decoded on demand.
  struct PageData {
    int width = 0;
    int height = 0;
    int dpi = 300;
    int rotation = 0;
    bool info_ok = false;
    std::shared_ptr<Jb2Image> mask;
    Pixmap background;
    Pixmap foreground;
    // Per-shape colours from an FGbz palette, when the page has one.
    std::vector<uint32_t> shape_colors;
    std::string text;
    std::vector<TextRun> runs;
    std::string error;
  };

  void ParseDirm(ByteSpan body);
  void ParseNavm(ByteSpan body);
  void BuildSinglePage(const Chunk& form);

  Component* FindComponent(const std::string& id);
  const Chunk* ResolveForm(Component* c);

  // Decodes the structural parts of a page (INFO and text) without
  // touching the image layers.
  void LoadPageInfo(int page);
  // Decodes the image layers. `subsample` renders the mask at reduced
  // resolution, which is both faster and anti-aliased.
  void LoadPageImage(int page, int subsample, PageData* out);

  std::shared_ptr<const Jb2Dict> DictForPage(const Chunk& page_form);

  std::vector<uint8_t> bytes_;
  Chunk root_;
  std::vector<Component> components_;
  std::vector<int> page_index_;  // page number -> component index
  std::vector<OutlineItem> outline_;
  std::string title_;
  bool bundled_ = false;
  bool multipage_ = false;

  // Shared dictionaries, keyed by component id.
  std::map<std::string, std::shared_ptr<const Jb2Dict>> dict_cache_;

  // Page info (size, text) is small and worth keeping for every page.
  std::vector<PageData> info_;

  // Rendering can run on worker threads; the caches above are shared.
  mutable std::mutex mu_;
};

}  // namespace vellum::djvu
