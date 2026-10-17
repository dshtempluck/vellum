// The navigation pane: page thumbnails and the document outline.
//
// Thumbnails are drawn from the same render cache as the main view, at a
// small fixed width, so opening the panel costs a handful of extra
// renders and nothing more. Bookmarks use a tree control, which gets
// keyboard navigation and expand/collapse for free.

#include "ui/app.h"  // brings in <windows.h>; must precede the headers below

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

namespace vellum::ui {
namespace {

constexpr wchar_t kClassName[] = L"VellumNavPane";

constexpr COLORREF kPaneBack = RGB(0xfa, 0xfa, 0xfb);
constexpr COLORREF kPaneEdge = RGB(0xd0, 0xd0, 0xd4);
constexpr COLORREF kTabBack = RGB(0xf0, 0xf0, 0xf2);
constexpr COLORREF kTabActive = RGB(0xff, 0xff, 0xff);
constexpr COLORREF kText = RGB(0x30, 0x30, 0x36);
constexpr COLORREF kCurrentPage = RGB(0x2c, 0x6a, 0xc4);

constexpr int kTabHeight = 26;
constexpr int kThumbWidth = 108;
constexpr int kThumbGap = 10;
constexpr int kLabelHeight = 16;

struct NavData {
  AppState* state = nullptr;
  HWND hwnd = nullptr;
  HWND tree = nullptr;
  HFONT font = nullptr;
  int scroll = 0;       // thumbnail scroll, in pixels
  int extent = 0;       // total thumbnail height
  int scale = 100;
  std::vector<int> thumb_y;   // top of each page's thumbnail
  std::vector<int> thumb_h;   // its height
  std::vector<HTREEITEM> outline_items;
  std::vector<int> outline_pages;
};

NavData* Data(HWND h) {
  return reinterpret_cast<NavData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

int S(NavData* d, int v) { return v * d->scale / 100; }

RECT ContentRect(NavData* d) {
  RECT rc;
  GetClientRect(d->hwnd, &rc);
  rc.top += S(d, kTabHeight);
  rc.right -= 1;  // leave the separator column
  return rc;
}

// Recomputes thumbnail positions for the current page sizes.
void MeasureThumbs(NavData* d) {
  AppState* s = d->state;
  d->thumb_y.clear();
  d->thumb_h.clear();
  d->extent = 0;
  if (!s->has_doc()) return;

  const int w = S(d, kThumbWidth);
  int y = S(d, kThumbGap);
  for (int i = 0; i < s->t().doc->page_count(); ++i) {
    const PageGeometry g = s->t().doc->geometry(i);
    const double ratio =
        (g.width_pt > 0) ? (g.height_pt / g.width_pt) : (792.0 / 612.0);
    const int h = std::max(16, static_cast<int>(w * ratio));
    d->thumb_y.push_back(y);
    d->thumb_h.push_back(h);
    y += h + S(d, kLabelHeight) + S(d, kThumbGap);
  }
  d->extent = y;
}

void UpdateThumbScroll(NavData* d) {
  const RECT c = ContentRect(d);
  const int view_h = c.bottom - c.top;
  d->scroll = std::clamp(d->scroll, 0, std::max(0, d->extent - view_h));

  SCROLLINFO si{};
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
  si.nMin = 0;
  si.nMax = std::max(0, d->extent - 1);
  si.nPage = static_cast<UINT>(std::max(1, view_h));
  si.nPos = d->scroll;
  SetScrollInfo(d->hwnd, SB_VERT, &si, TRUE);
}

void BlitThumb(HDC dc, const RECT& dest, const RenderedPage& page) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = page.pixmap.width();
  bi.bmiHeader.biHeight = -page.pixmap.height();
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 24;
  bi.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);
  StretchDIBits(dc, dest.left, dest.top, dest.right - dest.left,
                dest.bottom - dest.top, 0, 0, page.pixmap.width(),
                page.pixmap.height(), page.pixmap.row(0), &bi,
                DIB_RGB_COLORS, SRCCOPY);
}

void PaintThumbs(NavData* d, HDC dc, const RECT& content) {
  AppState* s = d->state;
  const int w = S(d, kThumbWidth);
  const int cx = (content.left + content.right) / 2;

  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ oldf = SelectObject(dc, d->font);

  std::vector<RenderCache::Key> wanted;
  for (int i = 0; i < static_cast<int>(d->thumb_y.size()); ++i) {
    const int top = content.top + d->thumb_y[i] - d->scroll;
    const int h = d->thumb_h[i];
    if (top + h + S(d, kLabelHeight) < content.top) continue;
    if (top > content.bottom) break;

    RECT box{cx - w / 2, top, cx + w / 2, top + h};

    const bool current = i == s->t().page;
    if (current) {
      RECT sel = box;
      InflateRect(&sel, 3, 3);
      HBRUSH b = CreateSolidBrush(kCurrentPage);
      FrameRect(dc, &sel, b);
      InflateRect(&sel, 1, 1);
      FrameRect(dc, &sel, b);
      DeleteObject(b);
    }

    RenderCache::Key key;
    key.page = i;
    key.width = w;
    key.height = h;
    key.rotation = s->t().layout.rotation();
    wanted.push_back(key);

    // Thumbnails are lower priority than the page being read.
    auto page = s->t().cache->Get(key, 50 + std::abs(i - s->t().page));
    if (page) {
      BlitThumb(dc, box, *page);
    } else {
      HBRUSH wb = CreateSolidBrush(RGB(0xff, 0xff, 0xff));
      FillRect(dc, &box, wb);
      DeleteObject(wb);
    }
    HBRUSH eb = CreateSolidBrush(kPaneEdge);
    FrameRect(dc, &box, eb);
    DeleteObject(eb);

    RECT label{content.left, box.bottom + 1, content.right,
               box.bottom + S(d, kLabelHeight)};
    SetTextColor(dc, current ? kCurrentPage : kText);
    const std::wstring n = std::to_wstring(i + 1);
    DrawTextW(dc, n.c_str(), -1, &label,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  SelectObject(dc, oldf);
}

void Paint(NavData* d, HDC target) {
  RECT client;
  GetClientRect(d->hwnd, &client);

  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, client.right, client.bottom);
  HGDIOBJ old = SelectObject(dc, bmp);

  HBRUSH back = CreateSolidBrush(kPaneBack);
  FillRect(dc, &client, back);
  DeleteObject(back);

  // Tab strip.
  const int tab_h = S(d, kTabHeight);
  RECT strip{0, 0, client.right, tab_h};
  HBRUSH tb = CreateSolidBrush(kTabBack);
  FillRect(dc, &strip, tb);
  DeleteObject(tb);

  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ oldf = SelectObject(dc, d->font);
  const int half = client.right / 2;
  const wchar_t* names[2] = {L"Thumbnails", L"Bookmarks"};
  for (int i = 0; i < 2; ++i) {
    RECT t{i * half, 0, (i + 1) * half, tab_h};
    const bool active =
        (i == 0) == (d->state->panel == NavPanel::kThumbnails);
    if (active) {
      HBRUSH ab = CreateSolidBrush(kTabActive);
      FillRect(dc, &t, ab);
      DeleteObject(ab);
    }
    SetTextColor(dc, active ? kCurrentPage : kText);
    DrawTextW(dc, names[i], -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  SelectObject(dc, oldf);

  HPEN pen = CreatePen(PS_SOLID, 1, kPaneEdge);
  HGDIOBJ op = SelectObject(dc, pen);
  MoveToEx(dc, 0, tab_h - 1, nullptr);
  LineTo(dc, client.right, tab_h - 1);
  MoveToEx(dc, client.right - 1, 0, nullptr);
  LineTo(dc, client.right - 1, client.bottom);
  SelectObject(dc, op);
  DeleteObject(pen);

  if (d->state->panel == NavPanel::kThumbnails)
    PaintThumbs(d, dc, ContentRect(d));

  BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
}

void AddOutline(NavData* d, const std::vector<OutlineItem>& items,
                HTREEITEM parent, int depth) {
  if (depth > 16) return;
  for (const OutlineItem& it : items) {
    const std::wstring title = Utf8ToWide(it.title);
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = const_cast<wchar_t*>(title.c_str());
    // lParam indexes our side table, so the page survives tree edits.
    ins.item.lParam = static_cast<LPARAM>(d->outline_pages.size());
    d->outline_pages.push_back(it.page);
    HTREEITEM h = reinterpret_cast<HTREEITEM>(
        SendMessageW(d->tree, TVM_INSERTITEMW, 0,
                     reinterpret_cast<LPARAM>(&ins)));
    if (!it.children.empty()) {
      AddOutline(d, it.children, h, depth + 1);
      if (it.open)
        SendMessageW(d->tree, TVM_EXPAND, TVE_EXPAND,
                     reinterpret_cast<LPARAM>(h));
    }
  }
}

void ShowPanel(NavData* d) {
  const bool books = d->state->panel == NavPanel::kBookmarks;
  ShowWindow(d->tree, books ? SW_SHOW : SW_HIDE);
  // The tree brings its own scrollbar; the thumbnail list uses the pane's.
  ShowScrollBar(d->hwnd, SB_VERT, !books);
  if (books) {
    RECT c = ContentRect(d);
    MoveWindow(d->tree, c.left, c.top, c.right - c.left, c.bottom - c.top,
               TRUE);
  }
  InvalidateRect(d->hwnd, nullptr, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  NavData* d = Data(hwnd);

  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      auto* data = new NavData();
      data->state = static_cast<AppState*>(cs->lpCreateParams);
      data->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(data));
      return TRUE;
    }

    case WM_CREATE: {
      d->scale = static_cast<int>(GetDpiForWindow(hwnd) * 100 / 96);
      d->font = CreateFontW(-S(d, 12), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                            0, L"Segoe UI");
      d->tree = CreateWindowExW(
          0, WC_TREEVIEWW, L"",
          WS_CHILD | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
              TVS_FULLROWSELECT | WS_VSCROLL,
          0, 0, 10, 10, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
      SendMessageW(d->tree, WM_SETFONT, reinterpret_cast<WPARAM>(d->font),
                   TRUE);
      ShowPanel(d);
      return 0;
    }

    case WM_DESTROY:
      if (d->font) DeleteObject(d->font);
      delete d;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return 0;

    case WM_SIZE:
      MeasureThumbs(d);
      UpdateThumbScroll(d);
      if (d->state->panel == NavPanel::kBookmarks) {
        RECT c = ContentRect(d);
        MoveWindow(d->tree, c.left, c.top, c.right - c.left,
                   c.bottom - c.top, TRUE);
      }
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      Paint(d, dc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_VSCROLL: {
      SCROLLINFO si{};
      si.cbSize = sizeof(si);
      si.fMask = SIF_ALL;
      GetScrollInfo(hwnd, SB_VERT, &si);
      int pos = si.nPos;
      switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= 40; break;
        case SB_LINEDOWN: pos += 40; break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: return 0;
      }
      d->scroll = pos;
      UpdateThumbScroll(d);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_MOUSEWHEEL: {
      if (d->state->panel != NavPanel::kThumbnails) break;
      const int delta = GET_WHEEL_DELTA_WPARAM(wp);
      d->scroll -= delta * 60 / WHEEL_DELTA;
      UpdateThumbScroll(d);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      RECT client;
      GetClientRect(hwnd, &client);

      if (pt.y < S(d, kTabHeight)) {
        d->state->panel = (pt.x < client.right / 2) ? NavPanel::kThumbnails
                                                    : NavPanel::kBookmarks;
        ShowPanel(d);
        return 0;
      }

      if (d->state->panel != NavPanel::kThumbnails) break;
      const RECT c = ContentRect(d);
      const int doc_y = pt.y - c.top + d->scroll;
      for (int i = 0; i < static_cast<int>(d->thumb_y.size()); ++i) {
        const int top = d->thumb_y[i];
        const int bottom = top + d->thumb_h[i] + S(d, kLabelHeight);
        if (doc_y >= top && doc_y < bottom) {
          PostMessageW(GetParent(hwnd), kMsgCommand, kCmdPageGoto, i + 1);
          break;
        }
      }
      return 0;
    }

    case WM_NOTIFY: {
      auto* nm = reinterpret_cast<NMHDR*>(lp);
      if (nm->hwndFrom == d->tree && nm->code == TVN_SELCHANGEDW) {
        auto* tv = reinterpret_cast<NMTREEVIEWW*>(lp);
        const size_t idx = static_cast<size_t>(tv->itemNew.lParam);
        if (idx < d->outline_pages.size()) {
          const int page = d->outline_pages[idx];
          if (page >= 0)
            PostMessageW(GetParent(hwnd), kMsgCommand, kCmdPageGoto,
                         page + 1);
        }
        return 0;
      }
      break;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreateNavPaneWindow(HWND parent, AppState* state) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
  }
  return CreateWindowExW(0, kClassName, L"",
                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                         0, 0, 10, 10, parent, nullptr,
                         GetModuleHandleW(nullptr), state);
}

void NavPaneDocumentChanged(HWND nav) {
  NavData* d = Data(nav);
  if (!d) return;
  d->scroll = 0;
  SendMessageW(d->tree, TVM_DELETEITEM, 0,
               reinterpret_cast<LPARAM>(TVI_ROOT));
  d->outline_pages.clear();
  if (d->state->has_doc())
    AddOutline(d, d->state->t().doc->outline(), TVI_ROOT, 0);

  // Land on whichever panel the document can actually fill.
  const bool has_outline =
      d->state->has_doc() && !d->state->t().doc->outline().empty();
  if (!has_outline && d->state->panel == NavPanel::kBookmarks)
    d->state->panel = NavPanel::kThumbnails;

  MeasureThumbs(d);
  UpdateThumbScroll(d);
  ShowPanel(d);
}

void NavPaneSyncPage(HWND nav) {
  NavData* d = Data(nav);
  if (!d || d->state->panel != NavPanel::kThumbnails) return;
  const int page = d->state->t().page;
  if (page < 0 || page >= static_cast<int>(d->thumb_y.size())) return;

  // Scroll the current page's thumbnail into view.
  const RECT c = ContentRect(d);
  const int view_h = c.bottom - c.top;
  const int top = d->thumb_y[page];
  const int bottom = top + d->thumb_h[page];
  if (top < d->scroll)
    d->scroll = top - 8;
  else if (bottom > d->scroll + view_h)
    d->scroll = bottom - view_h + 8;
  UpdateThumbScroll(d);
  InvalidateRect(nav, nullptr, FALSE);
}

}  // namespace vellum::ui
