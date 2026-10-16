// The main toolbar.
//
// Custom-drawn rather than a common control, for two reasons: it needs
// toggle groups and embedded page/zoom boxes laid out the way Acrobat's
// toolbar does, and drawing the glyphs with GDI primitives keeps the
// binary free of icon resources while staying sharp at every DPI.

#include "ui/app.h"  // brings in <windows.h>; must precede the headers below

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

namespace vellum::ui {
namespace {

constexpr wchar_t kClassName[] = L"VellumToolbar";

constexpr COLORREF kBarBack = RGB(0xf3, 0xf3, 0xf4);
constexpr COLORREF kBarEdge = RGB(0xd0, 0xd0, 0xd4);
constexpr COLORREF kGlyph = RGB(0x3a, 0x3a, 0x40);
constexpr COLORREF kGlyphDim = RGB(0xa8, 0xa8, 0xb0);
constexpr COLORREF kHover = RGB(0xe2, 0xe6, 0xef);
constexpr COLORREF kChecked = RGB(0xcf, 0xdc, 0xf2);
constexpr COLORREF kCheckedEdge = RGB(0x8c, 0xa8, 0xd8);

constexpr int kButton = 28;   // button side, scaled by DPI
constexpr int kSepWidth = 9;
constexpr int kBarHeight = 36;

enum class Glyph {
  kOpen,
  kHand,
  kSelect,
  kMarquee,
  kPrev,
  kNext,
  kFirst,
  kLast,
  kZoomIn,
  kZoomOut,
  kFitPage,
  kFitWidth,
  kRotateCW,
  kRotateCCW,
  kFind,
  kNavPane,
};

enum class ItemKind { kButton, kToggle, kSeparator, kPageBox, kZoomBox, kLabel };

struct Item {
  ItemKind kind;
  UINT cmd;
  Glyph glyph;
  const wchar_t* tip;
};

// Laid out in the order Acrobat groups them: file, tools, navigation,
// zoom, fit, rotate, find.
const Item kItems[] = {
    {ItemKind::kButton, kCmdFileOpen, Glyph::kOpen, L"Open  (Ctrl+O)"},
    {ItemKind::kToggle, kCmdViewNavPane, Glyph::kNavPane,
     L"Show/hide navigation pane  (F4)"},
    {ItemKind::kSeparator, kCmdNone, Glyph::kOpen, nullptr},

    {ItemKind::kToggle, kCmdToolHand, Glyph::kHand, L"Hand tool  (H)"},
    {ItemKind::kToggle, kCmdToolSelect, Glyph::kSelect,
     L"Select text  (V)"},
    {ItemKind::kToggle, kCmdToolMarquee, Glyph::kMarquee,
     L"Marquee zoom  (Z)"},
    {ItemKind::kSeparator, kCmdNone, Glyph::kOpen, nullptr},

    {ItemKind::kButton, kCmdPageFirst, Glyph::kFirst,
     L"First page  (Ctrl+Home)"},
    {ItemKind::kButton, kCmdPagePrev, Glyph::kPrev, L"Previous page"},
    {ItemKind::kPageBox, kCmdPageGoto, Glyph::kOpen, L"Page number"},
    {ItemKind::kLabel, kCmdNone, Glyph::kOpen, nullptr},
    {ItemKind::kButton, kCmdPageNext, Glyph::kNext, L"Next page"},
    {ItemKind::kButton, kCmdPageLast, Glyph::kLast, L"Last page  (Ctrl+End)"},
    {ItemKind::kSeparator, kCmdNone, Glyph::kOpen, nullptr},

    {ItemKind::kButton, kCmdZoomOut, Glyph::kZoomOut, L"Zoom out  (Ctrl+-)"},
    {ItemKind::kZoomBox, kCmdZoomTo, Glyph::kOpen, L"Zoom level"},
    {ItemKind::kButton, kCmdZoomIn, Glyph::kZoomIn, L"Zoom in  (Ctrl++)"},
    {ItemKind::kSeparator, kCmdNone, Glyph::kOpen, nullptr},

    {ItemKind::kToggle, kCmdZoomFitPage, Glyph::kFitPage,
     L"Fit one full page  (Ctrl+0)"},
    {ItemKind::kToggle, kCmdZoomFitWidth, Glyph::kFitWidth,
     L"Fit page width  (Ctrl+2)"},
    {ItemKind::kSeparator, kCmdNone, Glyph::kOpen, nullptr},

    {ItemKind::kButton, kCmdRotateCCW, Glyph::kRotateCCW,
     L"Rotate counterclockwise  (Ctrl+Shift+-)"},
    {ItemKind::kButton, kCmdRotateCW, Glyph::kRotateCW,
     L"Rotate clockwise  (Ctrl+Shift++)"},
    {ItemKind::kSeparator, kCmdNone, Glyph::kOpen, nullptr},

    {ItemKind::kButton, kCmdEditFind, Glyph::kFind, L"Find  (Ctrl+F)"},
};

constexpr int kItemCount = static_cast<int>(sizeof(kItems) / sizeof(kItems[0]));

struct BarData {
  AppState* state = nullptr;
  HWND hwnd = nullptr;
  HWND page_box = nullptr;
  HWND zoom_box = nullptr;
  HWND tooltip = nullptr;
  HFONT font = nullptr;
  RECT rects[kItemCount]{};
  int hot = -1;
  int pressed = -1;
  int scale = 100;  // DPI percentage
};

BarData* Data(HWND h) {
  return reinterpret_cast<BarData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

int S(BarData* d, int v) { return v * d->scale / 100; }

bool IsChecked(AppState* s, UINT cmd) {
  switch (cmd) {
    case kCmdToolHand: return s->tool == Tool::kHand;
    case kCmdToolSelect: return s->tool == Tool::kSelectText;
    case kCmdToolMarquee: return s->tool == Tool::kMarqueeZoom;
    case kCmdZoomFitPage:
      return s->layout.zoom_mode() == ZoomMode::kFitPage;
    case kCmdZoomFitWidth:
      return s->layout.zoom_mode() == ZoomMode::kFitWidth;
    case kCmdViewNavPane: return s->nav_visible;
    default: return false;
  }
}

bool IsEnabled(AppState* s, UINT cmd) {
  if (cmd == kCmdFileOpen) return true;
  if (!s->has_doc()) return false;
  switch (cmd) {
    case kCmdPageFirst:
    case kCmdPagePrev:
      return s->page > 0;
    case kCmdPageNext:
    case kCmdPageLast:
      return s->page + 1 < s->doc->page_count();
    default:
      return true;
  }
}

// --- glyph drawing ---------------------------------------------------------
// Everything is drawn into a square `r` with the current pen and brush
// already selected, so the caller controls enabled/disabled colour.

void Arrow(HDC dc, RECT r, int dir, bool bar) {
  const int cx = (r.left + r.right) / 2;
  const int cy = (r.top + r.bottom) / 2;
  const int w = (r.right - r.left) / 5;
  const int h = (r.bottom - r.top) / 4;
  POINT p[3];
  if (dir < 0) {
    p[0] = {cx + w / 2, cy - h};
    p[1] = {cx + w / 2, cy + h};
    p[2] = {cx - w, cy};
  } else {
    p[0] = {cx - w / 2, cy - h};
    p[1] = {cx - w / 2, cy + h};
    p[2] = {cx + w, cy};
  }
  Polygon(dc, p, 3);
  if (bar) {
    const int bx = dir < 0 ? cx - w - 2 : cx + w + 2;
    MoveToEx(dc, bx, cy - h, nullptr);
    LineTo(dc, bx, cy + h + 1);
  }
}

void PageOutline(HDC dc, RECT r) {
  Rectangle(dc, r.left, r.top, r.right, r.bottom);
}

void DrawGlyph(HDC dc, Glyph g, RECT r, COLORREF color) {
  HPEN pen = CreatePen(PS_SOLID,
                       std::max<int>(1, (r.right - r.left) / 14), color);
  HBRUSH brush = CreateSolidBrush(color);
  HGDIOBJ op = SelectObject(dc, pen);
  HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));

  const int cx = (r.left + r.right) / 2;
  const int cy = (r.top + r.bottom) / 2;
  const int s = (r.right - r.left);

  switch (g) {
    case Glyph::kOpen: {
      // A folder.
      const int x0 = r.left, x1 = r.right, y1 = r.bottom;
      const int y0 = r.top + s / 5;
      MoveToEx(dc, x0, y1, nullptr);
      LineTo(dc, x0, y0);
      LineTo(dc, x0 + s / 3, y0);
      LineTo(dc, x0 + s / 3 + s / 8, y0 - s / 6);
      LineTo(dc, x1, y0 - s / 6);
      LineTo(dc, x1, y1);
      LineTo(dc, x0, y1);
      break;
    }
    case Glyph::kHand: {
      // A simplified open hand.
      RoundRect(dc, cx - s / 4, cy - s / 8, cx + s / 4, cy + s / 2, s / 5,
                s / 5);
      for (int i = -1; i <= 1; ++i) {
        const int x = cx + i * s / 5;
        MoveToEx(dc, x, cy - s / 8, nullptr);
        LineTo(dc, x, cy - s / 2);
      }
      break;
    }
    case Glyph::kSelect: {
      // An I-beam.
      MoveToEx(dc, cx, cy - s / 2, nullptr);
      LineTo(dc, cx, cy + s / 2);
      MoveToEx(dc, cx - s / 5, cy - s / 2, nullptr);
      LineTo(dc, cx + s / 5 + 1, cy - s / 2);
      MoveToEx(dc, cx - s / 5, cy + s / 2, nullptr);
      LineTo(dc, cx + s / 5 + 1, cy + s / 2);
      break;
    }
    case Glyph::kMarquee: {
      // A dashed rectangle with a magnifier.
      HPEN dash = CreatePen(PS_DOT, 1, color);
      HGDIOBJ od = SelectObject(dc, dash);
      Rectangle(dc, r.left, r.top + s / 8, r.right - s / 4,
                r.bottom - s / 4);
      SelectObject(dc, od);
      DeleteObject(dash);
      Ellipse(dc, cx - s / 8, cy - s / 8, cx + s / 3, cy + s / 3);
      break;
    }
    case Glyph::kPrev: Arrow(dc, r, -1, false); break;
    case Glyph::kNext: Arrow(dc, r, +1, false); break;
    case Glyph::kFirst: Arrow(dc, r, -1, true); break;
    case Glyph::kLast: Arrow(dc, r, +1, true); break;
    case Glyph::kZoomIn:
    case Glyph::kZoomOut: {
      const int rad = s / 3;
      Ellipse(dc, cx - rad - s / 10, cy - rad - s / 10, cx + rad - s / 10,
              cy + rad - s / 10);
      MoveToEx(dc, cx + rad - s / 6, cy + rad - s / 6, nullptr);
      LineTo(dc, r.right, r.bottom);
      const int ix = cx - s / 10, iy = cy - s / 10;
      MoveToEx(dc, ix - rad / 2, iy, nullptr);
      LineTo(dc, ix + rad / 2, iy);
      if (g == Glyph::kZoomIn) {
        MoveToEx(dc, ix, iy - rad / 2, nullptr);
        LineTo(dc, ix, iy + rad / 2);
      }
      break;
    }
    case Glyph::kFitPage: {
      RECT p{cx - s / 3, r.top, cx + s / 3, r.bottom};
      PageOutline(dc, p);
      // Arrows meeting the top and bottom edges.
      MoveToEx(dc, cx, r.top + s / 5, nullptr);
      LineTo(dc, cx, r.bottom - s / 5);
      break;
    }
    case Glyph::kFitWidth: {
      RECT p{cx - s / 3, r.top + s / 8, cx + s / 3, r.bottom - s / 8};
      PageOutline(dc, p);
      MoveToEx(dc, r.left, cy, nullptr);
      LineTo(dc, r.right, cy);
      break;
    }
    case Glyph::kRotateCW:
    case Glyph::kRotateCCW: {
      const int rad = s / 3;
      Arc(dc, cx - rad, cy - rad, cx + rad, cy + rad, cx, cy - rad, cx, cy + rad);
      const int ax = (g == Glyph::kRotateCW) ? cx + rad : cx - rad;
      POINT t[3] = {{ax, cy - rad / 2}, {ax - rad / 3, cy + rad / 4},
                    {ax + rad / 3, cy + rad / 4}};
      SelectObject(dc, brush);
      Polygon(dc, t, 3);
      SelectObject(dc, GetStockObject(NULL_BRUSH));
      break;
    }
    case Glyph::kFind: {
      const int rad = s / 3;
      Ellipse(dc, cx - rad - s / 10, cy - rad - s / 10, cx + rad - s / 10,
              cy + rad - s / 10);
      MoveToEx(dc, cx + rad - s / 6, cy + rad - s / 6, nullptr);
      LineTo(dc, r.right, r.bottom);
      break;
    }
    case Glyph::kNavPane: {
      Rectangle(dc, r.left, r.top + s / 8, r.right, r.bottom - s / 8);
      const int split = r.left + s / 3;
      MoveToEx(dc, split, r.top + s / 8, nullptr);
      LineTo(dc, split, r.bottom - s / 8);
      break;
    }
  }

  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(pen);
  DeleteObject(brush);
}

// --- layout ----------------------------------------------------------------

void LayoutItems(BarData* d) {
  RECT client;
  GetClientRect(d->hwnd, &client);
  const int btn = S(d, kButton);
  const int h = client.bottom;
  int x = S(d, 6);
  const int y = (h - btn) / 2;

  for (int i = 0; i < kItemCount; ++i) {
    const Item& it = kItems[i];
    int w = btn;
    switch (it.kind) {
      case ItemKind::kSeparator: w = S(d, kSepWidth); break;
      case ItemKind::kPageBox: w = S(d, 44); break;
      case ItemKind::kZoomBox: w = S(d, 84); break;
      case ItemKind::kLabel: w = S(d, 42); break;
      default: break;
    }
    d->rects[i] = RECT{x, y, x + w, y + btn};
    x += w + S(d, 2);
  }

  // Position the embedded controls now that their slots are known.
  for (int i = 0; i < kItemCount; ++i) {
    const RECT& r = d->rects[i];
    if (kItems[i].kind == ItemKind::kPageBox && d->page_box) {
      MoveWindow(d->page_box, r.left, r.top + S(d, 3), r.right - r.left,
                 (r.bottom - r.top) - S(d, 6), TRUE);
    } else if (kItems[i].kind == ItemKind::kZoomBox && d->zoom_box) {
      MoveWindow(d->zoom_box, r.left, r.top + S(d, 2), r.right - r.left,
                 240, TRUE);
    }
  }
}

int HitItem(BarData* d, POINT pt) {
  for (int i = 0; i < kItemCount; ++i) {
    if (kItems[i].kind == ItemKind::kSeparator ||
        kItems[i].kind == ItemKind::kLabel ||
        kItems[i].kind == ItemKind::kPageBox ||
        kItems[i].kind == ItemKind::kZoomBox)
      continue;
    if (PtInRect(&d->rects[i], pt)) return i;
  }
  return -1;
}

void Paint(BarData* d, HDC target) {
  RECT client;
  GetClientRect(d->hwnd, &client);

  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, client.right, client.bottom);
  HGDIOBJ old = SelectObject(dc, bmp);

  HBRUSH back = CreateSolidBrush(kBarBack);
  FillRect(dc, &client, back);
  DeleteObject(back);

  // Bottom edge.
  HPEN edge = CreatePen(PS_SOLID, 1, kBarEdge);
  HGDIOBJ op = SelectObject(dc, edge);
  MoveToEx(dc, 0, client.bottom - 1, nullptr);
  LineTo(dc, client.right, client.bottom - 1);
  SelectObject(dc, op);
  DeleteObject(edge);

  AppState* s = d->state;
  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ oldf = SelectObject(dc, d->font);

  for (int i = 0; i < kItemCount; ++i) {
    const Item& it = kItems[i];
    const RECT& r = d->rects[i];

    if (it.kind == ItemKind::kSeparator) {
      HPEN p = CreatePen(PS_SOLID, 1, kBarEdge);
      HGDIOBJ o = SelectObject(dc, p);
      const int x = (r.left + r.right) / 2;
      MoveToEx(dc, x, r.top + 4, nullptr);
      LineTo(dc, x, r.bottom - 4);
      SelectObject(dc, o);
      DeleteObject(p);
      continue;
    }

    if (it.kind == ItemKind::kLabel) {
      // "of N", beside the page box.
      std::wstring text = L"";
      if (s->has_doc()) text = L"of " + std::to_wstring(s->doc->page_count());
      SetTextColor(dc, kGlyph);
      RECT t = r;
      DrawTextW(dc, text.c_str(), -1, &t,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      continue;
    }

    if (it.kind == ItemKind::kPageBox || it.kind == ItemKind::kZoomBox)
      continue;  // real child controls draw themselves

    const bool enabled = IsEnabled(s, it.cmd);
    const bool checked = IsChecked(s, it.cmd);
    const bool hot = d->hot == i && enabled;
    const bool down = d->pressed == i && hot;

    if (checked || hot) {
      HBRUSH b = CreateSolidBrush(down ? kCheckedEdge
                                       : (checked ? kChecked : kHover));
      HPEN p = CreatePen(PS_SOLID, 1, checked ? kCheckedEdge : kBarEdge);
      HGDIOBJ ob = SelectObject(dc, b);
      HGDIOBJ opn = SelectObject(dc, p);
      RoundRect(dc, r.left, r.top, r.right, r.bottom, 5, 5);
      SelectObject(dc, ob);
      SelectObject(dc, opn);
      DeleteObject(b);
      DeleteObject(p);
    }

    RECT g = r;
    InflateRect(&g, -S(d, 7), -S(d, 7));
    DrawGlyph(dc, it.glyph, g, enabled ? kGlyph : kGlyphDim);
  }

  SelectObject(dc, oldf);
  BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
}

void SyncBoxes(BarData* d) {
  AppState* s = d->state;
  if (d->page_box) {
    // Do not fight the user while they are typing in the box.
    if (GetFocus() != d->page_box) {
      const std::wstring t =
          s->has_doc() ? std::to_wstring(s->page + 1) : std::wstring();
      SetWindowTextW(d->page_box, t.c_str());
    }
    EnableWindow(d->page_box, s->has_doc());
  }
  if (d->zoom_box) {
    HWND edit = GetWindow(d->zoom_box, GW_CHILD);
    if (GetFocus() != d->zoom_box && GetFocus() != edit) {
      const std::wstring t =
          s->has_doc() ? FormatZoom(s->current.scale / s->dpi_scale)
                       : std::wstring();
      SetWindowTextW(d->zoom_box, t.c_str());
    }
    EnableWindow(d->zoom_box, s->has_doc());
  }
}

void AddTip(BarData* d, int index) {
  if (!d->tooltip || !kItems[index].tip) return;
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_SUBCLASS;
  ti.hwnd = d->hwnd;
  ti.uId = static_cast<UINT_PTR>(index);
  ti.rect = d->rects[index];
  ti.lpszText = const_cast<wchar_t*>(kItems[index].tip);
  SendMessageW(d->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  BarData* d = Data(hwnd);

  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      auto* data = new BarData();
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

      HINSTANCE inst = GetModuleHandleW(nullptr);
      d->page_box = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | ES_CENTER | ES_AUTOHSCROLL, 0, 0, 10, 10,
          hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kCmdPageGoto)),
          inst, nullptr);
      SendMessageW(d->page_box, WM_SETFONT,
                   reinterpret_cast<WPARAM>(d->font), TRUE);

      d->zoom_box = CreateWindowExW(
          0, L"COMBOBOX", L"",
          WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | WS_VSCROLL, 0, 0, 10, 240,
          hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kCmdZoomTo)),
          inst, nullptr);
      SendMessageW(d->zoom_box, WM_SETFONT,
                   reinterpret_cast<WPARAM>(d->font), TRUE);
      for (const wchar_t* z : {L"25%", L"50%", L"75%", L"100%", L"125%",
                               L"150%", L"200%", L"400%", L"800%"})
        SendMessageW(d->zoom_box, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(z));

      d->tooltip = CreateWindowExW(
          WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
          WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, hwnd, nullptr,
          inst, nullptr);
      SendMessageW(d->tooltip, TTM_SETMAXTIPWIDTH, 0, 400);

      LayoutItems(d);
      for (int i = 0; i < kItemCount; ++i) AddTip(d, i);
      SyncBoxes(d);
      return 0;
    }

    case WM_DESTROY:
      if (d->font) DeleteObject(d->font);
      delete d;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return 0;

    case WM_SIZE:
      LayoutItems(d);
      for (int i = 0; i < kItemCount; ++i) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = hwnd;
        ti.uId = static_cast<UINT_PTR>(i);
        ti.rect = d->rects[i];
        SendMessageW(d->tooltip, TTM_NEWTOOLRECTW, 0,
                     reinterpret_cast<LPARAM>(&ti));
      }
      InvalidateRect(hwnd, nullptr, FALSE);
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

    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      const int hit = HitItem(d, pt);
      if (hit != d->hot) {
        d->hot = hit;
        InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      d->hot = -1;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      d->pressed = HitItem(d, pt);
      if (d->pressed >= 0) {
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      const int hit = HitItem(d, pt);
      const int was = d->pressed;
      d->pressed = -1;
      ReleaseCapture();
      InvalidateRect(hwnd, nullptr, FALSE);
      if (was >= 0 && was == hit && IsEnabled(d->state, kItems[was].cmd))
        PostMessageW(GetParent(hwnd), kMsgCommand, kItems[was].cmd, 0);
      return 0;
    }

    case WM_COMMAND: {
      const UINT id = LOWORD(wp);
      const UINT code = HIWORD(wp);
      HWND from = reinterpret_cast<HWND>(lp);

      if (id == kCmdPageGoto && code == EN_KILLFOCUS) {
        wchar_t buf[32]{};
        GetWindowTextW(from, buf, 31);
        PostMessageW(GetParent(hwnd), kMsgCommand, kCmdPageGoto,
                     static_cast<LPARAM>(_wtoi(buf)));
        return 0;
      }
      if (id == kCmdZoomTo &&
          (code == CBN_SELENDOK || code == CBN_KILLFOCUS)) {
        wchar_t buf[32]{};
        if (code == CBN_SELENDOK) {
          const int sel = static_cast<int>(
              SendMessageW(from, CB_GETCURSEL, 0, 0));
          if (sel >= 0)
            SendMessageW(from, CB_GETLBTEXT, sel,
                         reinterpret_cast<LPARAM>(buf));
        } else {
          GetWindowTextW(from, buf, 31);
        }
        const int percent = _wtoi(buf);
        if (percent > 0)
          PostMessageW(GetParent(hwnd), kMsgCommand, kCmdZoomTo, percent);
        return 0;
      }
      break;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreateToolbarWindow(HWND parent, AppState* state) {
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
  const int dpi = static_cast<int>(GetDpiForWindow(parent));
  return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10,
                         kBarHeight * dpi / 96, parent, nullptr,
                         GetModuleHandleW(nullptr), state);
}

void ToolbarSyncState(HWND toolbar) {
  BarData* d = Data(toolbar);
  if (!d) return;
  SyncBoxes(d);
  InvalidateRect(toolbar, nullptr, FALSE);
}

int ToolbarHeight(HWND toolbar) {
  const int dpi = static_cast<int>(GetDpiForWindow(toolbar));
  return kBarHeight * dpi / 96;
}

}  // namespace vellum::ui
