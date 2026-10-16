#include "djvu/document.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "djvu/bzz.h"
#include "djvu/iw44.h"

namespace vellum::djvu {
namespace {

// Directory entry flags.
constexpr uint8_t kHasName = 0x80;
constexpr uint8_t kHasTitle = 0x40;
constexpr uint8_t kTypeMask = 0x3f;

constexpr int kTypePage = 1;

// Version-0 directories used a different flag layout.
constexpr uint8_t kV0IsPage = 1;
constexpr uint8_t kV0HasName = 2;
constexpr uint8_t kV0HasTitle = 4;

// Text zone types.
enum ZoneType {
  kZonePage = 1,
  kZoneColumn = 2,
  kZoneRegion = 3,
  kZoneParagraph = 4,
  kZoneLine = 5,
  kZoneWord = 6,
  kZoneCharacter = 7,
};

// A zone rectangle in DjVu page space: origin bottom-left, y upward.
struct ZoneRect {
  int xmin = 0, ymin = 0, xmax = 0, ymax = 0;
};

// Walks the zone tree of a TXTa/TXTz chunk, emitting one run per word.
//
// Coordinates and text offsets are delta-coded against the previous
// sibling, or against the parent for a first child, so each call returns
// what it decoded and the caller feeds that to the next sibling.
struct ZoneResult {
  ZoneRect rect;
  int text_start = 0;
  int text_length = 0;
};

ZoneResult DecodeZoneTree(ByteReader& r, const std::string& text, int depth,
                          const ZoneRect* parent, const ZoneResult* prev,
                          int parent_start, int page_w, int page_h,
                          std::vector<TextRun>* runs) {
  if (depth > 12) throw FormatError("DjVu text tree too deep");

  const int ztype = r.u8();
  if (ztype < kZonePage || ztype > kZoneCharacter)
    throw FormatError("DjVu bad text zone type");

  int x = static_cast<int>(r.u16()) - 0x8000;
  int y = static_cast<int>(r.u16()) - 0x8000;
  const int width = static_cast<int>(r.u16()) - 0x8000;
  const int height = static_cast<int>(r.u16()) - 0x8000;
  int text_start = static_cast<int>(r.u16()) - 0x8000;
  const int text_length = static_cast<int>(r.u24());

  if (prev) {
    if (ztype == kZonePage || ztype == kZoneParagraph || ztype == kZoneLine) {
      x = x + prev->rect.xmin;
      y = prev->rect.ymin - (y + height);
    } else {
      x = x + prev->rect.xmax;
      y = y + prev->rect.ymin;
    }
    text_start += prev->text_start + prev->text_length;
  } else if (parent) {
    x = x + parent->xmin;
    y = parent->ymax - (y + height);
    text_start += parent_start;
  }

  ZoneResult self;
  self.rect = ZoneRect{x, y, x + width, y + height};
  self.text_start = text_start;
  self.text_length = text_length;

  if (width <= 0 || height <= 0 || text_start < 0 ||
      static_cast<size_t>(text_start) + text_length > text.size())
    throw FormatError("DjVu corrupt text zone");

  const int nchildren = static_cast<int>(r.u24());
  if (nchildren < 0 || nchildren > 1 << 20)
    throw FormatError("DjVu implausible zone child count");

  if (ztype == kZoneWord && page_w > 0 && page_h > 0 && text_length > 0) {
    TextRun run;
    run.text = text.substr(static_cast<size_t>(text_start), text_length);
    run.char_offset = text_start;
    run.box.x0 = static_cast<float>(self.rect.xmin) / page_w;
    run.box.x1 = static_cast<float>(self.rect.xmax) / page_w;
    run.box.y0 = 1.0f - static_cast<float>(self.rect.ymax) / page_h;
    run.box.y1 = 1.0f - static_cast<float>(self.rect.ymin) / page_h;
    runs->push_back(std::move(run));
  }

  const ZoneResult* prev_child = nullptr;
  ZoneResult child;
  for (int i = 0; i < nchildren; ++i) {
    child = DecodeZoneTree(r, text, depth + 1, &self.rect, prev_child,
                           text_start, page_w, page_h, runs);
    prev_child = &child;
  }
  return self;
}

// Nearest-neighbour sample of a pixmap, used to lift the reduced-resolution
// background and foreground layers to the output grid.
inline void SamplePixmap(const Pixmap& src, int x, int y, int out_w, int out_h,
                         uint8_t* rgb) {
  if (src.empty()) {
    rgb[0] = rgb[1] = rgb[2] = 0xff;
    return;
  }
  int sx = out_w > 1 ? x * src.width() / out_w : 0;
  int sy = out_h > 1 ? y * src.height() / out_h : 0;
  sx = std::clamp(sx, 0, src.width() - 1);
  sy = std::clamp(sy, 0, src.height() - 1);
  const uint8_t* p = src.row(sy) + static_cast<size_t>(sx) * 3;
  rgb[0] = p[0];
  rgb[1] = p[1];
  rgb[2] = p[2];
}

}  // namespace

std::unique_ptr<DjvuDocument> DjvuDocument::Open(std::vector<uint8_t> bytes) {
  auto doc = std::unique_ptr<DjvuDocument>(new DjvuDocument());
  doc->bytes_ = std::move(bytes);
  ByteSpan file(doc->bytes_.data(), doc->bytes_.size());
  doc->root_ = OpenDjVuFile(file);

  if (doc->root_.is_form("DJVM")) {
    doc->multipage_ = true;
    // The directory must come first; everything else is addressed through
    // it, either by offset (bundled) or by name (indirect).
    ChunkReader r(doc->root_.body);
    Chunk c;
    while (r.Next(&c)) {
      if (c.is("DIRM")) {
        doc->ParseDirm(c.body);
      } else if (c.is("NAVM")) {
        doc->ParseNavm(c.body);
      }
    }
    if (doc->components_.empty())
      throw FormatError("DjVu multi-page document has no directory");
    if (!doc->bundled_)
      throw FormatError(
          "indirect DjVu documents are not supported: this file only holds "
          "an index, its pages live in separate files");
  } else if (doc->root_.is_form("DJVU")) {
    doc->BuildSinglePage(doc->root_);
  } else {
    throw FormatError("unrecognised DjVu container");
  }

  doc->info_.resize(doc->page_index_.size());
  return doc;
}

void DjvuDocument::BuildSinglePage(const Chunk& form) {
  Component c;
  c.id = "page1";
  c.type = kTypePage;
  c.form = form;
  c.resolved = true;
  components_.push_back(std::move(c));
  page_index_.push_back(0);
  bundled_ = true;
}

void DjvuDocument::ParseDirm(ByteSpan body) {
  ByteReader r(body);
  const uint8_t ver_byte = r.u8();
  bundled_ = (ver_byte & 0x80) != 0;
  const int version = ver_byte & 0x7f;
  if (version > 1) throw FormatError("DjVu directory version too new");

  const int nfiles = r.u16();
  if (nfiles <= 0) return;
  if (static_cast<size_t>(nfiles) > body.size())
    throw FormatError("DjVu directory file count implausible");

  components_.resize(nfiles);
  for (int i = 0; i < nfiles; ++i) {
    if (bundled_) {
      components_[i].offset = r.u32();
      if (version == 0) components_[i].size = r.u24();
      if (components_[i].offset == 0)
        throw FormatError("DjVu directory entry has no offset");
    }
  }

  // Everything from here on is BZZ-compressed.
  const std::vector<uint8_t> meta =
      BzzDecompress(body.subspan(r.pos(), body.size() - r.pos()));
  ByteReader m(ByteSpan(meta.data(), meta.size()));

  if (version > 0)
    for (int i = 0; i < nfiles; ++i) components_[i].size = m.u24();

  for (int i = 0; i < nfiles; ++i) {
    uint8_t flags = m.u8();
    if (version == 0) {
      // Translate the old layout into the current one.
      uint8_t f = (flags & kV0IsPage) ? 1 : 0;
      if (flags & kV0HasName) f |= kHasName;
      if (flags & kV0HasTitle) f |= kHasTitle;
      flags = f;
    }
    components_[i].type = flags & kTypeMask;
    // Names follow as a block of NUL-terminated strings.
    components_[i].size = components_[i].size;
    components_[i].id.clear();
    // Remember which optional strings this entry carries.
    components_[i].name = (flags & kHasName) ? "\x01" : "";
    components_[i].title = (flags & kHasTitle) ? "\x01" : "";
  }

  for (int i = 0; i < nfiles; ++i) {
    Component& c = components_[i];
    const bool has_name = c.name == "\x01";
    const bool has_title = c.title == "\x01";
    c.id = m.cstr();
    c.name = has_name ? m.cstr() : c.id;
    c.title = has_title ? m.cstr() : c.id;
    if (c.type == kTypePage) page_index_.push_back(i);
  }

  if (page_index_.empty())
    throw FormatError("DjVu document contains no pages");
}

void DjvuDocument::ParseNavm(ByteSpan body) {
  // NAVM is a BZZ-compressed, pre-order flattened bookmark tree: each
  // entry states how many immediate children follow it.
  std::vector<uint8_t> raw;
  try {
    raw = BzzDecompress(body);
  } catch (const FormatError&) {
    return;  // an unreadable outline is not worth failing the document for
  }
  ByteReader r(ByteSpan(raw.data(), raw.size()));

  struct Flat {
    int count = 0;
    OutlineItem item;
  };
  std::vector<Flat> flat;
  try {
    const int n = r.u16();
    flat.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      Flat f;
      const int lo = r.u8();
      const int hi = r.u8();
      f.count = (hi << 8) | lo;
      const int namesize = r.u16();
      ByteSpan name = r.take(namesize);
      f.item.title.assign(reinterpret_cast<const char*>(name.data()),
                          name.size());
      const int urlsize = static_cast<int>(r.u24());
      ByteSpan url = r.take(urlsize);
      const std::string u(reinterpret_cast<const char*>(url.data()),
                          url.size());
      // A destination is either "#page" or a component name.
      f.item.page = -1;
      if (!u.empty()) {
        std::string key = (u[0] == '#') ? u.substr(1) : u;
        if (!key.empty() &&
            key.find_first_not_of("0123456789") == std::string::npos) {
          // A bare number is a 1-based page number.
          f.item.page = std::atoi(key.c_str()) - 1;
        } else {
          for (size_t ci = 0; ci < page_index_.size(); ++ci) {
            const Component& c = components_[page_index_[ci]];
            if (c.id == key || c.name == key || c.title == key) {
              f.item.page = static_cast<int>(ci);
              break;
            }
          }
        }
      }
      flat.push_back(std::move(f));
    }
  } catch (const std::exception&) {
    if (flat.empty()) return;
  }

  // Rebuild the tree from the child counts.
  size_t cursor = 0;
  std::function<void(std::vector<OutlineItem>&, int)> take =
      [&](std::vector<OutlineItem>& into, int how_many) {
        for (int i = 0; i < how_many && cursor < flat.size(); ++i) {
          Flat f = flat[cursor++];
          OutlineItem item = std::move(f.item);
          item.open = false;
          take(item.children, f.count);
          into.push_back(std::move(item));
        }
      };
  // The top level is everything not consumed as somebody's child.
  while (cursor < flat.size()) {
    Flat f = flat[cursor++];
    OutlineItem item = std::move(f.item);
    take(item.children, f.count);
    outline_.push_back(std::move(item));
  }
}

DjvuDocument::Component* DjvuDocument::FindComponent(const std::string& id) {
  for (Component& c : components_)
    if (c.id == id || c.name == id) return &c;
  return nullptr;
}

const Chunk* DjvuDocument::ResolveForm(Component* c) {
  if (!c) return nullptr;
  if (c->resolved) return &c->form;
  if (!bundled_ || c->offset == 0 || c->offset + 8 > bytes_.size())
    return nullptr;

  // A bundled component is a complete FORM chunk at the recorded offset.
  ByteSpan at(bytes_.data() + c->offset,
              std::min(bytes_.size() - c->offset,
                       c->size ? c->size : bytes_.size() - c->offset));
  ChunkReader r(at);
  Chunk form;
  if (!r.Next(&form) || !form.is("FORM")) return nullptr;
  c->form = form;
  c->resolved = true;
  return &c->form;
}

std::shared_ptr<const Jb2Dict> DjvuDocument::DictForPage(
    const Chunk& page_form) {
  // A page names the dictionaries it needs with INCL chunks. In practice
  // there is at most one; if there are several, they chain so that each
  // inherits the previous one's shapes.
  std::shared_ptr<const Jb2Dict> dict;
  ChunkReader r(page_form.body);
  Chunk c;
  while (r.Next(&c)) {
    if (!c.is("INCL")) continue;
    std::string id(reinterpret_cast<const char*>(c.body.data()),
                   c.body.size());
    // INCL payloads are sometimes NUL- or newline-padded.
    while (!id.empty() && (id.back() == '\0' || id.back() == '\n' ||
                           id.back() == '\r' || id.back() == ' '))
      id.pop_back();
    if (id.empty()) continue;

    auto it = dict_cache_.find(id);
    if (it != dict_cache_.end()) {
      dict = it->second;
      continue;
    }

    Component* comp = FindComponent(id);
    const Chunk* form = ResolveForm(comp);
    if (!form) continue;

    std::shared_ptr<const Jb2Dict> built = dict;
    ChunkReader dr(form->body);
    Chunk dc;
    while (dr.Next(&dc)) {
      if (dc.is("Djbz")) {
        built = DecodeJb2Dict(dc.body, built);
      } else if (dc.is("INCL")) {
        // Dictionaries may themselves include others; one level is enough
        // for every file seen in practice, and deeper chains would need
        // cycle detection.
      }
    }
    if (built) {
      dict_cache_[id] = built;
      dict = built;
    }
  }
  return dict;
}

int DjvuDocument::page_count() const {
  return static_cast<int>(page_index_.size());
}

void DjvuDocument::LoadPageInfo(int page) {
  // Caller holds mu_.
  if (page < 0 || page >= static_cast<int>(page_index_.size())) return;
  PageData& d = info_[page];
  if (d.info_ok || !d.error.empty()) return;

  Component* comp = &components_[page_index_[page]];
  const Chunk* form = ResolveForm(comp);
  if (!form) {
    d.error = "page data not found";
    return;
  }

  ChunkReader r(form->body);
  Chunk c;
  ByteSpan text_chunk;
  bool text_compressed = false;
  while (r.Next(&c)) {
    if (c.is("INFO") && c.body.size() >= 4) {
      const uint8_t* b = c.body.data();
      const size_t n = c.body.size();
      d.width = (b[0] << 8) | b[1];
      d.height = (b[2] << 8) | b[3];
      d.dpi = 300;
      if (n >= 8 && b[7] != 0xff) d.dpi = (b[7] << 8) | b[6];
      if (d.dpi < 25 || d.dpi > 6000) d.dpi = 300;
      const int flags = (n >= 10) ? b[9] : 0;
      switch (flags & 0x7) {
        case 6: d.rotation = 90; break;
        case 2: d.rotation = 180; break;
        case 5: d.rotation = 270; break;
        default: d.rotation = 0; break;
      }
      d.info_ok = d.width > 0 && d.height > 0;
    } else if (c.is("TXTz")) {
      text_chunk = c.body;
      text_compressed = true;
    } else if (c.is("TXTa") && text_chunk.empty()) {
      text_chunk = c.body;
      text_compressed = false;
    }
  }

  if (!d.info_ok) {
    d.error = "page has no usable INFO chunk";
    return;
  }

  if (!text_chunk.empty()) {
    try {
      std::vector<uint8_t> raw;
      ByteSpan span = text_chunk;
      if (text_compressed) {
        raw = BzzDecompress(text_chunk);
        span = ByteSpan(raw.data(), raw.size());
      }
      ByteReader tr(span);
      const int textsize = static_cast<int>(tr.u24());
      if (textsize > 0) {
        ByteSpan t = tr.take(static_cast<size_t>(textsize));
        d.text.assign(reinterpret_cast<const char*>(t.data()), t.size());
        if (tr.remaining() > 0) {
          const int zversion = tr.u8();
          if (zversion == 1)
            DecodeZoneTree(tr, d.text, 0, nullptr, nullptr, 0, d.width,
                           d.height, &d.runs);
        }
      }
    } catch (const std::exception&) {
      // A damaged text layer costs search and selection on this page only.
      d.runs.clear();
    }
  }
}

PageGeometry DjvuDocument::geometry(int page) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto* self = const_cast<DjvuDocument*>(this);
  self->LoadPageInfo(page);
  PageGeometry g;
  if (page < 0 || page >= static_cast<int>(info_.size())) return g;
  const PageData& d = info_[page];
  if (!d.info_ok) {
    // Fall back to US Letter so the layout engine still has something
    // sensible to place.
    g.width_pt = 612;
    g.height_pt = 792;
    return g;
  }
  g.width_pt = 72.0 * d.width / d.dpi;
  g.height_pt = 72.0 * d.height / d.dpi;
  g.rotation = d.rotation;
  return g;
}

void DjvuDocument::LoadPageImage(int page, int subsample, PageData* out) {
  // Caller holds mu_ for the structural caches; decoding is done here.
  Component* comp = &components_[page_index_[page]];
  const Chunk* form = ResolveForm(comp);
  if (!form) {
    out->error = "page data not found";
    return;
  }

  std::shared_ptr<const Jb2Dict> dict = DictForPage(*form);

  Iw44Decoder background;
  Iw44Decoder foreground;
  bool have_bg = false;
  bool have_fg = false;

  ChunkReader r(form->body);
  Chunk c;
  while (r.Next(&c)) {
    try {
      if (c.is("Sjbz")) {
        out->mask = DecodeJb2Image(c.body, dict);
      } else if (c.is("BG44")) {
        background.AddChunk(c.body);
        have_bg = true;
      } else if (c.is("FG44")) {
        foreground.AddChunk(c.body);
        have_fg = true;
      }
    } catch (const std::exception& e) {
      // Keep whatever decoded; a missing layer still leaves a usable page.
      if (out->error.empty()) out->error = e.what();
    }
  }

  if (have_bg && background.ready()) {
    try {
      out->background = background.ToPixmap();
    } catch (const std::exception&) {
    }
  }
  if (have_fg && foreground.ready()) {
    try {
      out->foreground = foreground.ToPixmap();
    } catch (const std::exception&) {
    }
  }
  (void)subsample;
}

RenderedPage DjvuDocument::Render(int page, Size size, int rotation) {
  RenderedPage out;
  out.page = page;
  out.size = size;
  if (size.empty()) return out;

  PageData page_data;
  int pw = 0, ph = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (page < 0 || page >= static_cast<int>(page_index_.size())) {
      out.error = "page out of range";
      return out;
    }
    LoadPageInfo(page);
    pw = info_[page].width;
    ph = info_[page].height;
    page_data.width = pw;
    page_data.height = ph;
    page_data.rotation = info_[page].rotation;
    page_data.dpi = info_[page].dpi;
    if (!info_[page].error.empty()) out.error = info_[page].error;
    // Decoding the layers touches the shared dictionary cache, so it stays
    // under the lock. Pages are large enough that the serialisation costs
    // less than duplicating dictionaries per thread would.
    const int target_long = std::max(size.w, size.h);
    int subsample = 1;
    if (target_long > 0 && pw > 0) {
      const int page_long = std::max(pw, ph);
      subsample = std::clamp(page_long / std::max(1, target_long), 1, 12);
    }
    LoadPageImage(page, subsample, &page_data);
  }

  if (pw <= 0 || ph <= 0) {
    out.pixmap.Init(size.w, size.h);
    out.pixmap.Fill(0xff, 0xff, 0xff);
    if (out.error.empty()) out.error = "page size unknown";
    return out;
  }

  // Render at page orientation, then rotate on the way out.
  const bool swap = ((page_data.rotation + rotation) % 180) == 90;
  const int rw = swap ? size.h : size.w;
  const int rh = swap ? size.w : size.h;

  // Compose the mask at a resolution close to the output, which both
  // speeds things up and anti-aliases the ink.
  Bitmap mask;
  int mask_sub = 1;
  if (page_data.mask) {
    const int sub_x = std::max(1, page_data.mask->width / std::max(1, rw));
    const int sub_y = std::max(1, page_data.mask->height / std::max(1, rh));
    mask_sub = std::clamp(std::min(sub_x, sub_y), 1, 12);
    try {
      mask = page_data.mask->Compose(mask_sub);
    } catch (const std::exception& e) {
      if (out.error.empty()) out.error = e.what();
    }
  }

  Pixmap composed(rw, rh);
  const int max_gray = std::max(1, mask.grays() - 1);

  for (int y = 0; y < rh; ++y) {
    uint8_t* dst = composed.row(y);
    for (int x = 0; x < rw; ++x) {
      uint8_t bg[3] = {0xff, 0xff, 0xff};
      if (!page_data.background.empty())
        SamplePixmap(page_data.background, x, y, rw, rh, bg);

      uint8_t fg[3] = {0, 0, 0};
      if (!page_data.foreground.empty())
        SamplePixmap(page_data.foreground, x, y, rw, rh, fg);

      int cover = 0;
      if (!mask.empty()) {
        // mask rows run bottom-up, the output top-down.
        const int my = mask.rows() - 1 - (rh > 1 ? y * mask.rows() / rh : 0);
        const int mx = rw > 1 ? x * mask.cols() / rw : 0;
        if (my >= 0 && my < mask.rows() && mx >= 0 && mx < mask.cols())
          cover = mask.row(my)[mx];
      }

      uint8_t* p = dst + static_cast<size_t>(x) * 3;
      if (cover <= 0) {
        p[0] = bg[0];
        p[1] = bg[1];
        p[2] = bg[2];
      } else if (cover >= max_gray) {
        p[0] = fg[0];
        p[1] = fg[1];
        p[2] = fg[2];
      } else {
        // Partial coverage: blend foreground over background.
        for (int ch = 0; ch < 3; ++ch) {
          const int v = (fg[ch] * cover + bg[ch] * (max_gray - cover)) /
                        max_gray;
          p[ch] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
      }
    }
  }

  // Apply the document's own rotation plus the view rotation.
  const int total = ((page_data.rotation + rotation) % 360 + 360) % 360;
  if (total == 0) {
    out.pixmap = std::move(composed);
  } else {
    out.pixmap.Init(size.w, size.h);
    for (int y = 0; y < size.h; ++y) {
      uint8_t* dst = out.pixmap.row(y);
      for (int x = 0; x < size.w; ++x) {
        int sx = 0, sy = 0;
        switch (total) {
          case 90:
            sx = y;
            sy = rh - 1 - x;
            break;
          case 180:
            sx = rw - 1 - x;
            sy = rh - 1 - y;
            break;
          default:  // 270
            sx = rw - 1 - y;
            sy = x;
            break;
        }
        sx = std::clamp(sx, 0, rw - 1);
        sy = std::clamp(sy, 0, rh - 1);
        const uint8_t* s = composed.row(sy) + static_cast<size_t>(sx) * 3;
        uint8_t* p = dst + static_cast<size_t>(x) * 3;
        p[0] = s[0];
        p[1] = s[1];
        p[2] = s[2];
      }
    }
  }

  out.complete = page_data.mask != nullptr || !page_data.background.empty();
  if (!out.complete && out.error.empty())
    out.error = "page has no decodable image layers";
  return out;
}

std::vector<TextRun> DjvuDocument::Text(int page) {
  std::lock_guard<std::mutex> lock(mu_);
  LoadPageInfo(page);
  if (page < 0 || page >= static_cast<int>(info_.size())) return {};
  return info_[page].runs;
}

std::string DjvuDocument::format_name() const {
  return multipage_ ? "DjVu (multi-page)" : "DjVu";
}

}  // namespace vellum::djvu
