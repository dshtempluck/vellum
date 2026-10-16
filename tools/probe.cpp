// Command-line harness for the document back ends.
//
// Renders pages without any UI so the decoders can be exercised,
// timed and diffed on their own. Writes PPM because it needs no
// encoder and every image tool reads it.
//
//   vellum_probe info   <file>
//   vellum_probe text   <file> [page]
//   vellum_probe render <file> <page> <width> <out.ppm> [rotation]
//   vellum_probe all    <file> <width> <out-dir>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/bytes.h"
#include "doc/document.h"

namespace {

std::wstring Widen(const char* s) {
  std::wstring w;
  // The harness only ever gets paths from a console, so a byte-wise widen
  // is enough for ASCII and keeps this tool free of Windows headers.
  while (*s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s++)));
  return w;
}

bool WritePpm(const std::string& path, const vellum::Pixmap& pm) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", pm.width(), pm.height());
  for (int y = 0; y < pm.height(); ++y)
    std::fwrite(pm.row(y), 1, static_cast<size_t>(pm.width()) * 3, f);
  std::fclose(f);
  return true;
}

int Usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  vellum_probe info   <file>\n"
               "  vellum_probe text   <file> [page]\n"
               "  vellum_probe render <file> <page> <width> <out.ppm>\n"
               "  vellum_probe all    <file> <width> <out-dir>\n");
  return 2;
}

void PrintOutline(const std::vector<vellum::OutlineItem>& items, int depth) {
  for (const auto& it : items) {
    std::printf("%*s- %s", depth * 2 + 2, "", it.title.c_str());
    if (it.page >= 0) std::printf("  -> page %d", it.page + 1);
    std::printf("\n");
    PrintOutline(it.children, depth + 1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return Usage();
  const std::string cmd = argv[1];

  try {
    auto doc = vellum::OpenDocument(Widen(argv[2]));

    if (cmd == "info") {
      std::printf("format : %s\n", doc->format_name().c_str());
      std::printf("pages  : %d\n", doc->page_count());
      const int show = doc->page_count() < 8 ? doc->page_count() : 8;
      for (int i = 0; i < show; ++i) {
        const vellum::PageGeometry g = doc->geometry(i);
        std::printf("  page %-3d %8.2f x %8.2f pt   rotation %d\n", i + 1,
                    g.width_pt, g.height_pt, g.rotation);
      }
      if (show < doc->page_count()) std::printf("  ...\n");
      if (!doc->outline().empty()) {
        std::printf("outline:\n");
        PrintOutline(doc->outline(), 0);
      } else {
        std::printf("outline: (none)\n");
      }
      return 0;
    }

    if (cmd == "text") {
      const int page = (argc > 3) ? std::atoi(argv[3]) - 1 : 0;
      const auto runs = doc->Text(page);
      std::printf("page %d: %zu text runs\n", page + 1, runs.size());
      size_t shown = 0;
      std::string line;
      for (const auto& r : runs) {
        line += r.text;
        line += ' ';
        if (line.size() > 72) {
          std::printf("  %s\n", line.c_str());
          line.clear();
          if (++shown >= 12) break;
        }
      }
      if (!line.empty()) std::printf("  %s\n", line.c_str());
      return 0;
    }

    if (cmd == "render" && argc >= 6) {
      const int page = std::atoi(argv[3]) - 1;
      const int width = std::atoi(argv[4]);
      const vellum::PageGeometry g = doc->geometry(page);
      if (g.width_pt <= 0) {
        std::fprintf(stderr, "page %d has no size\n", page + 1);
        return 1;
      }
      const int rotation = (argc > 6) ? std::atoi(argv[6]) : 0;
      double pw = g.width_pt, ph = g.height_pt;
      if (rotation == 90 || rotation == 270) std::swap(pw, ph);
      const int height = static_cast<int>(width * ph / pw + 0.5);
      auto r = doc->Render(page, {width, height}, rotation);
      if (!r.error.empty())
        std::fprintf(stderr, "note: %s\n", r.error.c_str());
      if (r.pixmap.empty()) {
        std::fprintf(stderr, "render produced nothing\n");
        return 1;
      }
      if (!WritePpm(argv[5], r.pixmap)) {
        std::fprintf(stderr, "cannot write %s\n", argv[5]);
        return 1;
      }
      std::printf("wrote %s (%dx%d)%s\n", argv[5], r.pixmap.width(),
                  r.pixmap.height(), r.complete ? "" : " [incomplete]");
      return 0;
    }

    if (cmd == "all" && argc >= 5) {
      const int width = std::atoi(argv[3]);
      const std::string dir = argv[4];
      int ok = 0, failed = 0;
      for (int p = 0; p < doc->page_count(); ++p) {
        const vellum::PageGeometry g = doc->geometry(p);
        if (g.width_pt <= 0) {
          ++failed;
          continue;
        }
        const int height =
            static_cast<int>(width * g.height_pt / g.width_pt + 0.5);
        char name[512];
        std::snprintf(name, sizeof(name), "%s/page%03d.ppm", dir.c_str(),
                      p + 1);
        auto r = doc->Render(p, {width, height}, 0);
        if (r.pixmap.empty() || !r.complete) {
          std::printf("page %3d: FAILED %s\n", p + 1, r.error.c_str());
          ++failed;
          continue;
        }
        if (!r.error.empty())
          std::printf("page %3d: ok (%s)\n", p + 1, r.error.c_str());
        WritePpm(name, r.pixmap);
        ++ok;
      }
      std::printf("rendered %d pages, %d failed\n", ok, failed);
      return failed ? 1 : 0;
    }

    return Usage();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
