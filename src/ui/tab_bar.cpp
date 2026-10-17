// The document tab strip.
//
// Sits between the toolbar and the content, and appears only once a
// second document is open: a single document should not pay for chrome it
// does not need. Each tab owns a document and the whole reading position
// that goes with it, so switching back to a tab restores its page, zoom,
// layout, selection and search exactly as they were left.

#include "ui/app.h"  // brings in <windows.h>; must precede the headers below

#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

namespace vellum::ui {
namespace {

constexpr wchar_t kClassName[] = L"VellumTabBar";

constexpr COLORREF kStripBack = RGB(0xe4, 0xe4, 0xe8);
constexpr COLORREF kTabActive = RGB(0xff, 0xff, 0xff);
constexpr COLORREF kTabHover = RGB(0xf0, 0xf0, 0xf3);
constexpr COLORREF kEdge = RGB(0xc6, 0xc6, 0xcc);
constexpr COLORREF kText = RGB(0x25, 0x25, 0x2b);
constexpr COLORREF kTextDim = RGB(0x5a, 0x5a, 0x64);
constexpr COLORREF kCloseHot = RGB(0xd0, 0x4a, 0x42);

constexpr int kBarHeight = 30;
constexpr int kTabMax = 240;
constexpr int kTabMin = 92;
constexpr int kCloseSize = 16;

struct TabData {
  AppState* state = nullptr;
  HWND hwnd = nullptr;
  HFONT font = nullptr;
  int scale = 100;
  std::vector<RECT> rects;  // one per tab, in client coordinates
  int hot = -1;             // tab under the pointer
  int hot_close = -1;       // tab whose close button is under the pointer
  int pressed_close = -1;
};

TabData* Data(HWND h) {
  return reinterpret_cast<TabData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

int S(TabData* d, int v) { return v * d->scale / 100; }

void LayoutTabs(TabData* d) {
  RECT rc;
  GetClientRect(d->hwnd, &rc);
  const int count = static_cast<int>(d->state->tabs.size());
  d->rects.assign(static_cast<size_t>(std::max(0, count)), RECT{});
  if (count <= 0) return;

  // Share the width out, but never shrink a tab below the point where its
  // label is unreadable; past that the strip simply clips.
  const int avail = rc.right;
  int width = avail / count;
  width = std::clamp(width, S(d, kTabMin), S(d, kTabMax));

  int x = 0;
  for (int i = 0; i < count; ++i) {
    d->rects[i] = RECT{x, 0, x + width, rc.bottom};
    x += width;
  }
}

RECT CloseRect(TabData* d, int index) {
  const RECT& r = d->rects[index];
  const int size = S(d, kCloseSize);
  const int cy = (r.top + r.bottom - size) / 2;
  return RECT{r.right - size - S(d, 7), cy, r.right - S(d, 7), cy + size};
}

int HitTab(TabData* d, POINT pt, bool* on_close) {
  if (on_close) *on_close = false;
  for (size_t i = 0; i < d->rects.size(); ++i) {
    if (!PtInRect(&d->rects[i], pt)) continue;
    if (on_close) {
      RECT c = CloseRect(d, static_cast<int>(i));
      InflateRect(&c, 2, 2);
      *on_close = PtInRect(&c, pt) != 0;
    }
    return static_cast<int>(i);
  }
  return -1;
}

void DrawClose(HDC dc, RECT r, bool hot) {
  if (hot) {
    HBRUSH b = CreateSolidBrush(kCloseHot);
    RECT bg = r;
    InflateRect(&bg, 1, 1);
    FillRect(dc, &bg, b);
    DeleteObject(b);
  }
  HPEN pen = CreatePen(PS_SOLID, 1, hot ? RGB(0xff, 0xff, 0xff) : kTextDim);
  HGDIOBJ old = SelectObject(dc, pen);
  const int pad = (r.right - r.left) / 4;
  MoveToEx(dc, r.left + pad, r.top + pad, nullptr);
  LineTo(dc, r.right - pad, r.bottom - pad);
  MoveToEx(dc, r.right - pad - 1, r.top + pad, nullptr);
  LineTo(dc, r.left + pad - 1, r.bottom - pad);
  SelectObject(dc, old);
  DeleteObject(pen);
}

void Paint(TabData* d, HDC target) {
  RECT client;
  GetClientRect(d->hwnd, &client);

  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, client.right, client.bottom);
  HGDIOBJ old = SelectObject(dc, bmp);

  HBRUSH back = CreateSolidBrush(kStripBack);
  FillRect(dc, &client, back);
  DeleteObject(back);

  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ oldf = SelectObject(dc, d->font);

  for (size_t i = 0; i < d->rects.size(); ++i) {
    const RECT& r = d->rects[i];
    if (r.right <= 0 || r.left >= client.right) continue;
    const bool active = static_cast<int>(i) == d->state->active;
    const bool hot = static_cast<int>(i) == d->hot;

    if (active || hot) {
      HBRUSH b = CreateSolidBrush(active ? kTabActive : kTabHover);
      FillRect(dc, &r, b);
      DeleteObject(b);
    }

    // Separator between tabs, skipped either side of the active one so it
    // reads as a single raised surface.
    if (!active) {
      HPEN pen = CreatePen(PS_SOLID, 1, kEdge);
      HGDIOBJ op = SelectObject(dc, pen);
      MoveToEx(dc, r.right - 1, r.top + S(d, 6), nullptr);
      LineTo(dc, r.right - 1, r.bottom - S(d, 6));
      SelectObject(dc, op);
      DeleteObject(pen);
    }

    const RECT close = CloseRect(d, static_cast<int>(i));
    RECT label{r.left + S(d, 10), r.top, close.left - S(d, 4), r.bottom};
    SetTextColor(dc, active ? kText : kTextDim);
    const std::wstring text = d->state->tabs[i]->Label();
    DrawTextW(dc, text.c_str(), -1, &label,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

    // The close button shows on the active tab and on hover, so the strip
    // stays quiet when there is nothing to act on.
    if (active || hot)
      DrawClose(dc, close, static_cast<int>(i) == d->hot_close);
  }

  // The line under the strip, broken where the active tab meets it.
  HPEN pen = CreatePen(PS_SOLID, 1, kEdge);
  HGDIOBJ op = SelectObject(dc, pen);
  const int y = client.bottom - 1;
  int cut_from = 0, cut_to = 0;
  if (d->state->active >= 0 &&
      d->state->active < static_cast<int>(d->rects.size())) {
    cut_from = d->rects[d->state->active].left;
    cut_to = d->rects[d->state->active].right;
  }
  MoveToEx(dc, 0, y, nullptr);
  LineTo(dc, cut_from, y);
  MoveToEx(dc, cut_to, y, nullptr);
  LineTo(dc, client.right, y);
  SelectObject(dc, op);
  DeleteObject(pen);

  SelectObject(dc, oldf);
  BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
}

void Post(TabData* d, UINT cmd, LPARAM param) {
  PostMessageW(GetParent(d->hwnd), kMsgCommand, cmd, param);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  TabData* d = Data(hwnd);

  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      auto* data = new TabData();
      data->state = static_cast<AppState*>(cs->lpCreateParams);
      data->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(data));
      return TRUE;
    }

    case WM_CREATE:
      d->scale = static_cast<int>(GetDpiForWindow(hwnd) * 100 / 96);
      d->font = CreateFontW(-S(d, 12), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                            0, L"Segoe UI");
      LayoutTabs(d);
      return 0;

    case WM_DESTROY:
      if (d->font) DeleteObject(d->font);
      delete d;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return 0;

    case WM_SIZE:
      LayoutTabs(d);
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
      bool on_close = false;
      const int hit = HitTab(d, pt, &on_close);
      const int close = on_close ? hit : -1;
      if (hit != d->hot || close != d->hot_close) {
        d->hot = hit;
        d->hot_close = close;
        InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      d->hot = d->hot_close = -1;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      bool on_close = false;
      const int hit = HitTab(d, pt, &on_close);
      if (hit < 0) return 0;
      if (on_close) {
        d->pressed_close = hit;
        SetCapture(hwnd);
        return 0;
      }
      // Switching happens on press, which is what every tab strip does.
      Post(d, kCmdTabSelect, hit);
      return 0;
    }

    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      const int was = d->pressed_close;
      d->pressed_close = -1;
      if (was < 0) return 0;
      ReleaseCapture();
      bool on_close = false;
      if (HitTab(d, pt, &on_close) == was && on_close)
        Post(d, kCmdTabCloseAt, was + 1);
      return 0;
    }

    case WM_MBUTTONUP: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      const int hit = HitTab(d, pt, nullptr);
      // Middle click closes, as it does in a browser.
      if (hit >= 0) Post(d, kCmdTabCloseAt, hit + 1);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreateTabBarWindow(HWND parent, AppState* state) {
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
  return CreateWindowExW(0, kClassName, L"", WS_CHILD, 0, 0, 10, 10, parent,
                         nullptr, GetModuleHandleW(nullptr), state);
}

void TabBarSync(HWND bar) {
  TabData* d = Data(bar);
  if (!d) return;
  LayoutTabs(d);
  InvalidateRect(bar, nullptr, FALSE);
}

int TabBarHeight(HWND bar) {
  const int dpi = static_cast<int>(GetDpiForWindow(bar));
  return kBarHeight * dpi / 96;
}

}  // namespace vellum::ui
