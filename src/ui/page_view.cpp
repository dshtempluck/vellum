// The scrolling page viewport.
//
// Owns scroll position, paints whatever the render cache has ready, and
// implements the three pointer tools. Painting is double-buffered and
// pages are blitted straight from the cache's BGR bitmaps, so a scroll
// costs one StretchDIBits per visible page and nothing else.

#include "ui/app.h"  // brings in <windows.h>; must precede the headers below

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace vellum::ui {
namespace {

constexpr wchar_t kClassName[] = L"VellumPageView";

// Chrome colours, close to Acrobat's neutral grey workspace.
constexpr COLORREF kBackdrop = RGB(0x53, 0x53, 0x57);
constexpr COLORREF kPageBorder = RGB(0x24, 0x24, 0x26);
constexpr COLORREF kPlaceholder = RGB(0xff, 0xff, 0xff);
constexpr COLORREF kSelection = RGB(0x33, 0x66, 0xcc);
constexpr COLORREF kFindCurrent = RGB(0xff, 0x9b, 0x21);

struct ViewData {
  AppState* state = nullptr;
  HWND hwnd = nullptr;

  // Drag state for the active tool.
  bool dragging = false;
  POINT drag_start{0, 0};
  POINT drag_scroll{0, 0};
  RECT marquee{0, 0, 0, 0};
  PageHit sel_anchor;

  // Remembered so a resize can keep the same place in the document.
  double anchor_fraction = 0.0;
};

ViewData* Data(HWND h) {
  return reinterpret_cast<ViewData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

Size ClientSize(HWND h) {
  RECT rc{};
  GetClientRect(h, &rc);
  return Size{rc.right - rc.left, rc.bottom - rc.top};
}

// The viewport in document space.
Rect ViewRect(ViewData* d) {
  const Size c = ClientSize(d->hwnd);
  return Rect::FromXYWH(d->state->t().scroll.x, d->state->t().scroll.y, c.w, c.h);
}

// Centres the viewport horizontally on the sheet holding `p`.
//
// A document with mixed page orientations is as wide as its widest page,
// so without this a portrait page in a document that also holds a
// landscape one would sit off to the side with a band of empty backdrop
// beside it. In a two-up layout the whole spread must be centred, not its
// left-hand page, which is why this works on the sheet: every page laid
// out at the same top edge belongs to the same one.
void CenterOnPage(ViewData* d, const PagePlacement* p) {
  if (!p) return;
  Rect sheet = p->rect;
  for (const PagePlacement& other : d->state->t().current.pages)
    if (other.rect.y0 == p->rect.y0) sheet = sheet.Union(other.rect);
  const Size c = ClientSize(d->hwnd);
  d->state->t().scroll.x = sheet.x0 + (sheet.width() - c.w) / 2;
}

void ClampScroll(ViewData* d) {
  const Size c = ClientSize(d->hwnd);
  AppState* s = d->state;
  const int max_x = std::max(0, s->t().current.extent.w - c.w);
  const int max_y = std::max(0, s->t().current.extent.h - c.h);
  s->t().scroll.x = std::clamp<long>(s->t().scroll.x, 0, max_x);
  s->t().scroll.y = std::clamp<long>(s->t().scroll.y, 0, max_y);
  // Centre the document when it is narrower than the viewport, which is
  // what makes a zoomed-out page sit in the middle rather than top-left.
  if (s->t().current.extent.w < c.w)
    s->t().scroll.x = -(c.w - s->t().current.extent.w) / 2;
  if (s->t().current.extent.h < c.h)
    s->t().scroll.y = -(c.h - s->t().current.extent.h) / 2;
}

void UpdateScrollBars(ViewData* d) {
  const Size c = ClientSize(d->hwnd);
  AppState* s = d->state;

  SCROLLINFO si{};
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;

  si.nMin = 0;
  si.nMax = std::max(0, s->t().current.extent.h - 1);
  si.nPage = static_cast<UINT>(std::max(1, c.h));
  si.nPos = std::max<long>(0, s->t().scroll.y);
  SetScrollInfo(d->hwnd, SB_VERT, &si, TRUE);

  si.nMax = std::max(0, s->t().current.extent.w - 1);
  si.nPage = static_cast<UINT>(std::max(1, c.w));
  si.nPos = std::max<long>(0, s->t().scroll.x);
  SetScrollInfo(d->hwnd, SB_HORZ, &si, TRUE);
}

void Relayout(ViewData* d, bool keep_position) {
  AppState* s = d->state;
  if (!s->has_doc()) {
    s->t().current = Layout();
    UpdateScrollBars(d);
    InvalidateRect(d->hwnd, nullptr, TRUE);
    return;
  }

  // Remember where we are as a fraction of the current page, so a zoom
  // or window resize lands on the same content.
  double frac = 0.0;
  if (keep_position) {
    const PagePlacement* p = s->t().current.Find(s->t().page);
    if (p && p->rect.height() > 0)
      frac = static_cast<double>(s->t().scroll.y - p->rect.y0) / p->rect.height();
  }

  s->t().current = s->t().layout.Build(ClientSize(d->hwnd), s->t().page);

  const PagePlacement* p = s->t().current.Find(s->t().page);
  if (keep_position && p)
    s->t().scroll.y = p->rect.y0 + static_cast<long>(frac * p->rect.height());
  CenterOnPage(d, p);

  ClampScroll(d);
  UpdateScrollBars(d);
  InvalidateRect(d->hwnd, nullptr, TRUE);
}

void NotifyFrame(ViewData* d, UINT cmd) {
  PostMessageW(GetParent(d->hwnd), kMsgCommand, cmd, 0);
}

// Keeps the frame's idea of the current page in step with scrolling.
void SyncCurrentPage(ViewData* d) {
  AppState* s = d->state;
  if (!s->has_doc()) return;
  const int page = s->t().current.DominantPage(ViewRect(d));
  if (page >= 0 && page != s->t().page) {
    s->t().page = page;
    NotifyFrame(d, kCmdNone);  // frame refreshes its page box and nav pane
  }
}

void ScrollBy(ViewData* d, int dx, int dy) {
  AppState* s = d->state;
  const POINT before = s->t().scroll;
  s->t().scroll.x += dx;
  s->t().scroll.y += dy;
  ClampScroll(d);
  if (before.x == s->t().scroll.x && before.y == s->t().scroll.y) return;
  UpdateScrollBars(d);
  SyncCurrentPage(d);
  InvalidateRect(d->hwnd, nullptr, TRUE);
}

// In a one-page-at-a-time mode, scrolling past an edge turns the page.
bool TurnPageIfAtEdge(ViewData* d, int dir) {
  AppState* s = d->state;
  if (s->t().layout.is_continuous()) return false;
  const Size c = ClientSize(d->hwnd);
  if (dir > 0) {
    if (s->t().scroll.y + c.h < s->t().current.extent.h - 1) return false;
    if (s->t().page + 1 >= s->t().doc->page_count()) return false;
    NotifyFrame(d, kCmdPageNext);
    return true;
  }
  if (s->t().scroll.y > 0) return false;
  if (s->t().page <= 0) return false;
  NotifyFrame(d, kCmdPagePrev);
  return true;
}

PageHit HitTest(ViewData* d, POINT client) {
  AppState* s = d->state;
  PageHit hit;
  const int dx = client.x + s->t().scroll.x;
  const int dy = client.y + s->t().scroll.y;
  for (const PagePlacement& p : s->t().current.pages) {
    if (!p.rect.Contains(dx, dy)) continue;
    hit.page = p.page;
    hit.x = static_cast<float>(dx - p.rect.x0) / std::max(1, p.rect.width());
    hit.y = static_cast<float>(dy - p.rect.y0) / std::max(1, p.rect.height());
    break;
  }
  return hit;
}

// Maps a normalised page rectangle to client coordinates.
RECT PageRectToClient(ViewData* d, const PagePlacement& p, const RectF& box) {
  AppState* s = d->state;
  RECT r;
  r.left = p.rect.x0 + static_cast<int>(box.x0 * p.rect.width()) - s->t().scroll.x;
  r.right = p.rect.x0 + static_cast<int>(box.x1 * p.rect.width()) - s->t().scroll.x;
  r.top = p.rect.y0 + static_cast<int>(box.y0 * p.rect.height()) - s->t().scroll.y;
  r.bottom =
      p.rect.y0 + static_cast<int>(box.y1 * p.rect.height()) - s->t().scroll.y;
  return r;
}

// Finds the index of the text run nearest a normalised point, for
// click-and-drag selection.
int NearestRun(const std::vector<TextRun>& runs, float x, float y) {
  int best = -1;
  double best_d = 1e30;
  for (size_t i = 0; i < runs.size(); ++i) {
    const RectF& b = runs[i].box;
    // Vertical distance dominates: clicking in a margin should pick the
    // run on that line, not one several lines away but horizontally near.
    const double cy = (b.y0 + b.y1) * 0.5;
    const double dy = std::abs(y - cy);
    double dx = 0;
    if (x < b.x0) dx = b.x0 - x;
    else if (x > b.x1) dx = x - b.x1;
    const double d = dy * 8.0 + dx;
    if (d < best_d) {
      best_d = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void DrawPlaceholder(HDC dc, const RECT& rc) {
  HBRUSH b = CreateSolidBrush(kPlaceholder);
  FillRect(dc, &rc, b);
  DeleteObject(b);
}

// Blits a cached page bitmap, scaling if it was rendered at another size.
void BlitPage(HDC dc, const RECT& dest, const RenderedPage& page) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = page.pixmap.width();
  // Negative height means a top-down bitmap, which is how Pixmap stores
  // its rows.
  bi.bmiHeader.biHeight = -page.pixmap.height();
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 24;
  bi.bmiHeader.biCompression = BI_RGB;

  const int dw = dest.right - dest.left;
  const int dh = dest.bottom - dest.top;
  if (dw <= 0 || dh <= 0) return;

  const bool exact =
      dw == page.pixmap.width() && dh == page.pixmap.height();
  SetStretchBltMode(dc, exact ? COLORONCOLOR : HALFTONE);
  if (!exact) SetBrushOrgEx(dc, 0, 0, nullptr);

  StretchDIBits(dc, dest.left, dest.top, dw, dh, 0, 0, page.pixmap.width(),
                page.pixmap.height(), page.pixmap.row(0), &bi, DIB_RGB_COLORS,
                SRCCOPY);
}

void Paint(ViewData* d, HDC target, const RECT& client) {
  AppState* s = d->state;

  // Double buffer: pages, highlights and the backdrop all land in one
  // off-screen bitmap so scrolling never flickers.
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, client.right, client.bottom);
  HGDIOBJ old = SelectObject(dc, bmp);

  HBRUSH back = CreateSolidBrush(kBackdrop);
  FillRect(dc, &client, back);
  DeleteObject(back);

  if (!s->has_doc()) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xc8, 0xc8, 0xcc));
    HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                          L"Segoe UI");
    HGDIOBJ oldf = SelectObject(dc, f);
    RECT t = client;
    DrawTextW(dc, L"Open a PDF or DjVu document  (Ctrl+O)", -1, &t,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldf);
    DeleteObject(f);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return;
  }

  const Rect view = ViewRect(d);
  // Ask for pages a screen beyond the viewport so scrolling finds them
  // already rendered.
  const Rect prefetch = view.Inflate(view.height());

  std::vector<RenderCache::Key> wanted;
  HPEN border = CreatePen(PS_SOLID, 1, kPageBorder);
  HGDIOBJ oldpen = SelectObject(dc, border);

  for (const PagePlacement& p : s->t().current.pages) {
    if (!p.rect.Intersects(prefetch)) continue;

    RenderCache::Key key;
    key.page = p.page;
    key.width = p.rect.width();
    key.height = p.rect.height();
    key.rotation = s->t().layout.rotation();
    wanted.push_back(key);

    const bool on_screen = p.rect.Intersects(view);
    // Visible pages first, then the ones just outside, nearest first.
    const int priority =
        on_screen ? 0 : 1 + std::abs(p.rect.y0 - static_cast<int>(s->t().scroll.y));

    RECT dest;
    dest.left = p.rect.x0 - s->t().scroll.x;
    dest.top = p.rect.y0 - s->t().scroll.y;
    dest.right = p.rect.x1 - s->t().scroll.x;
    dest.bottom = p.rect.y1 - s->t().scroll.y;

    if (!on_screen) {
      s->t().cache->Get(key, priority);  // warm the cache, draw nothing
      continue;
    }

    auto page = s->t().cache->Get(key, priority);
    if (page) {
      BlitPage(dc, dest, *page);
    } else if (auto any = s->t().cache->GetAnySize(p.page, key.rotation)) {
      // Show the wrong size scaled rather than a blank sheet; it will be
      // replaced as soon as the right size arrives.
      BlitPage(dc, dest, *any);
    } else {
      DrawPlaceholder(dc, dest);
    }

    // Page edge.
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, dest.left - 1, dest.top - 1, dest.right + 1,
              dest.bottom + 1);

    // Search hits on this page, then the selection on top.
    if (!s->t().hits.empty()) {
      const auto& runs = s->TextFor(p.page);
      for (size_t hi = 0; hi < s->t().hits.size(); ++hi) {
        const SearchHit& h = s->t().hits[hi];
        if (h.page != p.page) continue;
        const bool current = static_cast<int>(hi) == s->t().hit_index;
        for (int ri = h.first_run;
             ri <= h.last_run && ri < static_cast<int>(runs.size()); ++ri) {
          RECT r = PageRectToClient(d, p, runs[ri].box);
          HBRUSH hb = CreateSolidBrush(current ? kFindCurrent
                                               : RGB(0xff, 0xe3, 0x8a));
          // Highlights multiply into the page so the text stays legible.
          const int save = SaveDC(dc);
          SetROP2(dc, R2_MASKPEN);
          FillRect(dc, &r, hb);
          RestoreDC(dc, save);
          DeleteObject(hb);
        }
      }
    }

    if (s->t().selection.page == p.page && !s->t().selection.empty()) {
      const auto& runs = s->TextFor(p.page);
      for (int ri = s->t().selection.first_run;
           ri <= s->t().selection.last_run && ri < static_cast<int>(runs.size());
           ++ri) {
        RECT r = PageRectToClient(d, p, runs[ri].box);
        HBRUSH hb = CreateSolidBrush(kSelection);
        const int save = SaveDC(dc);
        SetROP2(dc, R2_MASKPEN);
        FillRect(dc, &r, hb);
        RestoreDC(dc, save);
        DeleteObject(hb);
      }
    }
  }

  SelectObject(dc, oldpen);
  DeleteObject(border);

  // Marquee zoom rectangle.
  if (d->dragging && s->tool == Tool::kMarqueeZoom) {
    HPEN p = CreatePen(PS_DOT, 1, RGB(0xff, 0xff, 0xff));
    HGDIOBJ op = SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetROP2(dc, R2_XORPEN);
    Rectangle(dc, d->marquee.left, d->marquee.top, d->marquee.right,
              d->marquee.bottom);
    SetROP2(dc, R2_COPYPEN);
    SelectObject(dc, op);
    DeleteObject(p);
  }

  // Drop requests for pages that have scrolled far away.
  s->t().cache->DropPendingExcept(wanted);

  BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
}

void OnLButtonDown(ViewData* d, POINT pt) {
  AppState* s = d->state;
  SetFocus(d->hwnd);
  if (!s->has_doc()) return;

  d->dragging = true;
  d->drag_start = pt;
  d->drag_scroll = s->t().scroll;
  SetCapture(d->hwnd);

  if (s->tool == Tool::kMarqueeZoom) {
    d->marquee = RECT{pt.x, pt.y, pt.x, pt.y};
  } else if (s->tool == Tool::kSelectText) {
    d->sel_anchor = HitTest(d, pt);
    s->t().selection.clear();
    InvalidateRect(d->hwnd, nullptr, FALSE);
  }
}

void OnMouseMove(ViewData* d, POINT pt) {
  AppState* s = d->state;
  if (!d->dragging) return;

  switch (s->tool) {
    case Tool::kHand: {
      const int dx = d->drag_start.x - pt.x;
      const int dy = d->drag_start.y - pt.y;
      s->t().scroll.x = d->drag_scroll.x + dx;
      s->t().scroll.y = d->drag_scroll.y + dy;
      ClampScroll(d);
      UpdateScrollBars(d);
      SyncCurrentPage(d);
      InvalidateRect(d->hwnd, nullptr, FALSE);
      break;
    }
    case Tool::kMarqueeZoom:
      d->marquee.right = pt.x;
      d->marquee.bottom = pt.y;
      InvalidateRect(d->hwnd, nullptr, FALSE);
      break;
    case Tool::kSelectText: {
      const PageHit now = HitTest(d, pt);
      if (!d->sel_anchor.valid() || !now.valid() ||
          now.page != d->sel_anchor.page)
        break;
      const auto& runs = s->TextFor(now.page);
      if (runs.empty()) break;
      const int a = NearestRun(runs, d->sel_anchor.x, d->sel_anchor.y);
      const int b = NearestRun(runs, now.x, now.y);
      if (a < 0 || b < 0) break;
      s->t().selection.page = now.page;
      s->t().selection.first_run = std::min(a, b);
      s->t().selection.last_run = std::max(a, b);
      InvalidateRect(d->hwnd, nullptr, FALSE);
      break;
    }
  }
}

void OnLButtonUp(ViewData* d, POINT pt) {
  AppState* s = d->state;
  if (!d->dragging) return;
  d->dragging = false;
  ReleaseCapture();

  if (s->tool != Tool::kMarqueeZoom) {
    InvalidateRect(d->hwnd, nullptr, FALSE);
    return;
  }

  RECT m = d->marquee;
  if (m.right < m.left) std::swap(m.left, m.right);
  if (m.bottom < m.top) std::swap(m.top, m.bottom);
  const int mw = m.right - m.left;
  const int mh = m.bottom - m.top;
  const Size c = ClientSize(d->hwnd);

  if (mw < 8 || mh < 8) {
    // A click rather than a drag: step the zoom, the way Acrobat does.
    const bool out = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    NotifyFrame(d, out ? kCmdZoomOut : kCmdZoomIn);
    (void)pt;
    InvalidateRect(d->hwnd, nullptr, TRUE);
    return;
  }

  // Zoom so the dragged rectangle fills the viewport, keeping its centre.
  const double factor =
      std::min(static_cast<double>(c.w) / mw, static_cast<double>(c.h) / mh);
  const double center_x = s->t().scroll.x + (m.left + m.right) * 0.5;
  const double center_y = s->t().scroll.y + (m.top + m.bottom) * 0.5;

  s->t().layout.set_zoom_mode(ZoomMode::kCustom);
  s->t().layout.set_zoom(s->t().current.scale * factor);
  const double ratio = s->t().layout.zoom() / std::max(1e-9, s->t().current.scale);

  Relayout(d, false);
  s->t().scroll.x = static_cast<long>(center_x * ratio - c.w * 0.5);
  s->t().scroll.y = static_cast<long>(center_y * ratio - c.h * 0.5);
  ClampScroll(d);
  UpdateScrollBars(d);
  SyncCurrentPage(d);
  NotifyFrame(d, kCmdNone);
  InvalidateRect(d->hwnd, nullptr, TRUE);
}

void OnWheel(ViewData* d, int delta, UINT keys, POINT screen_pt) {
  AppState* s = d->state;
  if (!s->has_doc()) return;

  if (keys & MK_CONTROL) {
    // Zoom about the pointer, so the content under the cursor stays put.
    POINT pt = screen_pt;
    ScreenToClient(d->hwnd, &pt);
    const double before = s->t().current.scale;
    const double step = delta > 0 ? 1.15 : 1.0 / 1.15;
    s->t().layout.set_zoom_mode(ZoomMode::kCustom);
    s->t().layout.set_zoom(before * step);
    const double doc_x = s->t().scroll.x + pt.x;
    const double doc_y = s->t().scroll.y + pt.y;
    Relayout(d, false);
    const double ratio = s->t().current.scale / std::max(1e-9, before);
    s->t().scroll.x = static_cast<long>(doc_x * ratio - pt.x);
    s->t().scroll.y = static_cast<long>(doc_y * ratio - pt.y);
    ClampScroll(d);
    UpdateScrollBars(d);
    SyncCurrentPage(d);
    NotifyFrame(d, kCmdNone);
    InvalidateRect(d->hwnd, nullptr, TRUE);
    return;
  }

  UINT lines = 3;
  SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
  const int amount = static_cast<int>(lines) * 24 * delta / WHEEL_DELTA;
  if (TurnPageIfAtEdge(d, delta > 0 ? -1 : 1)) return;
  ScrollBy(d, 0, -amount);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  ViewData* d = Data(hwnd);

  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      auto* data = new ViewData();
      data->state = static_cast<AppState*>(cs->lpCreateParams);
      data->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(data));
      return TRUE;
    }

    case WM_DESTROY:
      delete d;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return 0;

    case WM_ERASEBKGND:
      return 1;  // fully repainted in WM_PAINT

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      Paint(d, dc, rc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_SIZE:
      Relayout(d, true);
      return 0;

    case WM_VSCROLL:
    case WM_HSCROLL: {
      const bool vert = msg == WM_VSCROLL;
      SCROLLINFO si{};
      si.cbSize = sizeof(si);
      si.fMask = SIF_ALL;
      GetScrollInfo(hwnd, vert ? SB_VERT : SB_HORZ, &si);
      int pos = si.nPos;
      const int page_step = static_cast<int>(si.nPage);
      switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= 48; break;
        case SB_LINEDOWN: pos += 48; break;
        case SB_PAGEUP: pos -= page_step; break;
        case SB_PAGEDOWN: pos += page_step; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = si.nMin; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: return 0;
      }
      if (vert)
        ScrollBy(d, 0, pos - d->state->t().scroll.y);
      else
        ScrollBy(d, pos - d->state->t().scroll.x, 0);
      return 0;
    }

    case WM_MOUSEWHEEL:
      OnWheel(d, GET_WHEEL_DELTA_WPARAM(wp), GET_KEYSTATE_WPARAM(wp),
              POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
      return 0;

    case WM_LBUTTONDOWN:
      OnLButtonDown(d, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
      return 0;

    case WM_MOUSEMOVE:
      OnMouseMove(d, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
      return 0;

    case WM_LBUTTONUP:
      OnLButtonUp(d, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
      return 0;

    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        LPCWSTR id = IDC_ARROW;
        switch (d->state->tool) {
          case Tool::kHand: id = d->dragging ? IDC_SIZEALL : IDC_HAND; break;
          case Tool::kSelectText: id = IDC_IBEAM; break;
          case Tool::kMarqueeZoom: id = IDC_CROSS; break;
        }
        SetCursor(LoadCursorW(nullptr, id));
        return TRUE;
      }
      break;

    case WM_KEYDOWN: {
      AppState* s = d->state;
      const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const Size c = ClientSize(hwnd);
      switch (wp) {
        case VK_NEXT:
          if (!TurnPageIfAtEdge(d, 1)) ScrollBy(d, 0, c.h - 40);
          return 0;
        case VK_PRIOR:
          if (!TurnPageIfAtEdge(d, -1)) ScrollBy(d, 0, -(c.h - 40));
          return 0;
        case VK_DOWN: ScrollBy(d, 0, 48); return 0;
        case VK_UP: ScrollBy(d, 0, -48); return 0;
        case VK_RIGHT: ScrollBy(d, 48, 0); return 0;
        case VK_LEFT: ScrollBy(d, -48, 0); return 0;
        case VK_HOME:
          if (ctrl) NotifyFrame(d, kCmdPageFirst);
          else ScrollBy(d, 0, -s->t().current.extent.h);
          return 0;
        case VK_END:
          if (ctrl) NotifyFrame(d, kCmdPageLast);
          else ScrollBy(d, 0, s->t().current.extent.h);
          return 0;
        case VK_SPACE:
          if (!TurnPageIfAtEdge(d, 1)) ScrollBy(d, 0, c.h - 40);
          return 0;
        // Tool shortcuts and Escape are plain keys, so they are handled
        // here rather than in the global accelerator table, where they
        // would be stolen from the find and page boxes.
        case 'H': NotifyFrame(d, kCmdToolHand); return 0;
        case 'V': NotifyFrame(d, kCmdToolSelect); return 0;
        case 'Z': NotifyFrame(d, kCmdToolMarquee); return 0;
        case VK_ESCAPE: NotifyFrame(d, kCmdFindClose); return 0;
      }
      break;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreatePageViewWindow(HWND parent, AppState* state) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = nullptr;  // set per tool in WM_SETCURSOR
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
  }
  return CreateWindowExW(
      0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL, 0,
      0, 10, 10, parent, nullptr, GetModuleHandleW(nullptr), state);
}

void PageViewDocumentChanged(HWND view) {
  ViewData* d = Data(view);
  if (!d) return;
  d->state->t().scroll = POINT{0, 0};
  Relayout(d, false);
}

void PageViewRelayout(HWND view) {
  ViewData* d = Data(view);
  if (!d) return;
  Relayout(d, true);
}

void PageViewScrollToPage(HWND view, int page) {
  ViewData* d = Data(view);
  if (!d || !d->state->has_doc()) return;
  AppState* s = d->state;
  s->t().page = std::clamp(page, 0, s->t().doc->page_count() - 1);

  // In a paged mode the layout holds only the current sheet, so it has to
  // be rebuilt before the page exists to scroll to.
  if (!s->t().layout.is_continuous()) {
    s->t().scroll = POINT{0, 0};
    Relayout(d, false);
    return;
  }

  // A fit mode sizes pages against the anchor page. When pages differ in
  // size -- a folder of pictures, or a document with a landscape plate --
  // arriving at a new page means refitting to it.
  if (s->t().layout.zoom_mode() != ZoomMode::kCustom) Relayout(d, false);

  const PagePlacement* p = s->t().current.Find(s->t().page);
  if (!p) {
    Relayout(d, false);
    return;
  }
  s->t().scroll.y = p->rect.y0 - LayoutEngine::kPageGap;
  CenterOnPage(d, p);
  ClampScroll(d);
  UpdateScrollBars(d);
  InvalidateRect(view, nullptr, TRUE);
}

void PageViewEnsureHitVisible(HWND view, const SearchHit& hit) {
  ViewData* d = Data(view);
  if (!d || !d->state->has_doc()) return;
  AppState* s = d->state;

  if (s->t().page != hit.page || !s->t().layout.is_continuous())
    PageViewScrollToPage(view, hit.page);

  const PagePlacement* p = s->t().current.Find(hit.page);
  if (!p) return;
  const auto& runs = s->TextFor(hit.page);
  if (hit.first_run >= static_cast<int>(runs.size())) return;

  const RectF& box = runs[hit.first_run].box;
  const Size c = ClientSize(view);
  const int y = p->rect.y0 + static_cast<int>(box.y0 * p->rect.height());
  const int x = p->rect.x0 + static_cast<int>(box.x0 * p->rect.width());

  // Put the hit a third of the way down, which reads better than pinning
  // it to the top edge.
  if (y < s->t().scroll.y + 40 || y > s->t().scroll.y + c.h - 40)
    s->t().scroll.y = y - c.h / 3;
  if (x < s->t().scroll.x || x > s->t().scroll.x + c.w - 40)
    s->t().scroll.x = x - c.w / 3;

  ClampScroll(d);
  UpdateScrollBars(d);
  InvalidateRect(view, nullptr, TRUE);
}

}  // namespace vellum::ui
