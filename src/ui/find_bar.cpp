// The find bar.
//
// A strip below the page view holding the search box, previous/next
// buttons, a match-case toggle and a result count. Searching itself lives
// in the frame window, which owns the document; this is only the input.

#include "ui/app.h"  // brings in <windows.h>; must precede the headers below

#include <windowsx.h>

#include <string>

namespace vellum::ui {
namespace {

constexpr wchar_t kClassName[] = L"VellumFindBar";

constexpr COLORREF kBack = RGB(0xf3, 0xf3, 0xf4);
constexpr COLORREF kEdge = RGB(0xd0, 0xd0, 0xd4);
constexpr COLORREF kText = RGB(0x30, 0x30, 0x36);
constexpr COLORREF kNoMatch = RGB(0xc0, 0x2a, 0x2a);

enum : UINT {
  kIdEdit = 1,
  kIdPrev,
  kIdNext,
  kIdCase,
  kIdClose,
};

struct FindData {
  AppState* state = nullptr;
  HWND hwnd = nullptr;
  HWND edit = nullptr;
  HWND prev = nullptr;
  HWND next = nullptr;
  HWND match_case = nullptr;
  HWND close = nullptr;
  HFONT font = nullptr;
  int scale = 100;
};

FindData* Data(HWND h) {
  return reinterpret_cast<FindData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

int S(FindData* d, int v) { return v * d->scale / 100; }

void LayoutChildren(FindData* d) {
  RECT rc;
  GetClientRect(d->hwnd, &rc);
  const int h = S(d, 22);
  const int y = (rc.bottom - h) / 2;
  int x = S(d, 8);

  MoveWindow(d->edit, x, y, S(d, 220), h, TRUE);
  x += S(d, 226);
  MoveWindow(d->prev, x, y, S(d, 28), h, TRUE);
  x += S(d, 30);
  MoveWindow(d->next, x, y, S(d, 28), h, TRUE);
  x += S(d, 36);
  MoveWindow(d->match_case, x, y, S(d, 96), h, TRUE);

  MoveWindow(d->close, rc.right - S(d, 30), y, S(d, 22), h, TRUE);
}

void Paint(FindData* d, HDC dc) {
  RECT rc;
  GetClientRect(d->hwnd, &rc);
  HBRUSH b = CreateSolidBrush(kBack);
  FillRect(dc, &rc, b);
  DeleteObject(b);

  HPEN p = CreatePen(PS_SOLID, 1, kEdge);
  HGDIOBJ op = SelectObject(dc, p);
  MoveToEx(dc, 0, 0, nullptr);
  LineTo(dc, rc.right, 0);
  SelectObject(dc, op);
  DeleteObject(p);

  // Result count, to the right of the match-case box.
  AppState* s = d->state;
  RECT t;
  GetWindowRect(d->match_case, &t);
  MapWindowPoints(nullptr, d->hwnd, reinterpret_cast<POINT*>(&t), 2);
  RECT label{t.right + S(d, 12), 0, rc.right - S(d, 40), rc.bottom};

  std::wstring text;
  if (!s->find_text.empty()) {
    if (s->hits.empty()) {
      text = L"No matches";
    } else {
      text = std::to_wstring(s->hit_index + 1) + L" of " +
             std::to_wstring(s->hits.size()) + L" matches";
    }
  }

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, (!s->find_text.empty() && s->hits.empty()) ? kNoMatch
                                                              : kText);
  HGDIOBJ oldf = SelectObject(dc, d->font);
  DrawTextW(dc, text.c_str(), -1, &label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(dc, oldf);
}

void Submit(FindData* d, bool forward) {
  wchar_t buf[512]{};
  GetWindowTextW(d->edit, buf, 511);
  d->state->find_text = buf;
  d->state->find_case =
      SendMessageW(d->match_case, BM_GETCHECK, 0, 0) == BST_CHECKED;
  PostMessageW(GetParent(d->hwnd), kMsgCommand,
               forward ? kCmdFindNext : kCmdFindPrev, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  FindData* d = Data(hwnd);

  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      auto* data = new FindData();
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

      auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                      UINT id, DWORD ex = 0) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 0, 0, 10, 10, hwnd,
                                 reinterpret_cast<HMENU>(
                                     static_cast<UINT_PTR>(id)),
                                 inst, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
        return h;
      };

      d->edit = make(L"EDIT", L"", ES_AUTOHSCROLL, kIdEdit, WS_EX_CLIENTEDGE);
      d->prev = make(L"BUTTON", L"↑", BS_PUSHBUTTON, kIdPrev);
      d->next = make(L"BUTTON", L"↓", BS_PUSHBUTTON, kIdNext);
      d->match_case =
          make(L"BUTTON", L"Match case", BS_AUTOCHECKBOX, kIdCase);
      d->close = make(L"BUTTON", L"✕", BS_PUSHBUTTON, kIdClose);

      LayoutChildren(d);
      return 0;
    }

    case WM_DESTROY:
      if (d->font) DeleteObject(d->font);
      delete d;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return 0;

    case WM_SIZE:
      LayoutChildren(d);
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

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
      static HBRUSH bg = CreateSolidBrush(kBack);
      return reinterpret_cast<LRESULT>(bg);
    }

    case WM_COMMAND: {
      const UINT id = LOWORD(wp);
      const UINT code = HIWORD(wp);
      switch (id) {
        case kIdNext: Submit(d, true); return 0;
        case kIdPrev: Submit(d, false); return 0;
        case kIdClose:
          PostMessageW(GetParent(hwnd), kMsgCommand, kCmdFindClose, 0);
          return 0;
        case kIdCase:
          Submit(d, true);
          return 0;
        case kIdEdit:
          if (code == EN_CHANGE) {
            // Search as you type, the way a modern find bar behaves.
            wchar_t buf[512]{};
            GetWindowTextW(d->edit, buf, 511);
            d->state->find_text = buf;
            PostMessageW(GetParent(hwnd), kMsgCommand, kCmdEditFind, 1);
          }
          return 0;
      }
      break;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreateFindBarWindow(HWND parent, AppState* state) {
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
  return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0,
                         10, 10, parent, nullptr, GetModuleHandleW(nullptr),
                         state);
}

void FindBarFocus(HWND bar) {
  FindData* d = Data(bar);
  if (!d) return;
  SetFocus(d->edit);
  SendMessageW(d->edit, EM_SETSEL, 0, -1);
  InvalidateRect(bar, nullptr, FALSE);
}

}  // namespace vellum::ui
