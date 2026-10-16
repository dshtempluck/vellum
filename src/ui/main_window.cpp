// The frame window: menus, commands, child layout and search.
//
// The frame owns the document and the application state; the toolbar, nav
// pane, page view and find bar are views onto it that ask for commands by
// posting kMsgCommand. Keeping every state change in one place is what
// makes the toolbar, the nav pane and the status bar agree with each
// other at all times.

#include "ui/app.h"  // brings in <windows.h>; must precede the headers below

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "core/bytes.h"

namespace vellum::ui {

int ToolbarHeight(HWND toolbar);

namespace {

constexpr wchar_t kClassName[] = L"VellumMainWindow";
constexpr wchar_t kAppName[] = L"Vellum";

constexpr int kStatusHeight = 22;
constexpr int kFindHeight = 34;
constexpr int kSplitterWidth = 4;
constexpr int kMinNavWidth = 120;

struct FrameData {
  AppState state;
  HWND hwnd = nullptr;
  HWND toolbar = nullptr;
  HWND nav = nullptr;
  HWND view = nullptr;
  HWND find = nullptr;
  HWND status = nullptr;
  HMENU menu = nullptr;
  bool find_visible = false;
  bool dragging_splitter = false;
  // Window placement remembered across a full-screen round trip.
  WINDOWPLACEMENT saved_placement{};
};

FrameData* Data(HWND h) {
  return reinterpret_cast<FrameData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

// --- menu ------------------------------------------------------------------

HMENU BuildMenu() {
  HMENU bar = CreateMenu();

  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, kCmdFileOpen, L"&Open...\tCtrl+O");
  AppendMenuW(file, MF_STRING, kCmdFileClose, L"&Close\tCtrl+W");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kCmdFileProperties, L"P&roperties...\tCtrl+D");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kCmdFileExit, L"E&xit");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

  HMENU edit = CreatePopupMenu();
  AppendMenuW(edit, MF_STRING, kCmdEditCopy, L"&Copy\tCtrl+C");
  AppendMenuW(edit, MF_STRING, kCmdEditSelectAll, L"Select &All\tCtrl+A");
  AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(edit, MF_STRING, kCmdEditFind, L"&Find...\tCtrl+F");
  AppendMenuW(edit, MF_STRING, kCmdFindNext, L"Find &Next\tF3");
  AppendMenuW(edit, MF_STRING, kCmdFindPrev, L"Find Pre&vious\tShift+F3");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"&Edit");

  HMENU layout = CreatePopupMenu();
  AppendMenuW(layout, MF_STRING, kCmdLayoutSingle, L"&Single Page");
  AppendMenuW(layout, MF_STRING, kCmdLayoutContinuous,
              L"Single Page &Continuous");
  AppendMenuW(layout, MF_STRING, kCmdLayoutTwoPage, L"&Two Page View");
  AppendMenuW(layout, MF_STRING, kCmdLayoutTwoPageContinuous,
              L"Two Page Co&ntinuous");
  AppendMenuW(layout, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layout, MF_STRING, kCmdLayoutCoverPage,
              L"Show Co&ver Page in Two Page View");

  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, kCmdZoomIn, L"Zoom &In\tCtrl++");
  AppendMenuW(view, MF_STRING, kCmdZoomOut, L"Zoom &Out\tCtrl+-");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kCmdZoomActual, L"&Actual Size\tCtrl+1");
  AppendMenuW(view, MF_STRING, kCmdZoomFitPage, L"&Fit Page\tCtrl+0");
  AppendMenuW(view, MF_STRING, kCmdZoomFitWidth, L"Fit &Width\tCtrl+2");
  AppendMenuW(view, MF_STRING, kCmdZoomFitVisible, L"Fit &Visible\tCtrl+3");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(layout),
              L"Page &Layout");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kCmdRotateCW,
              L"&Rotate Clockwise\tCtrl+Shift++");
  AppendMenuW(view, MF_STRING, kCmdRotateCCW,
              L"Rotate &Counterclockwise\tCtrl+Shift+-");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kCmdViewNavPane,
              L"&Navigation Pane\tF4");
  AppendMenuW(view, MF_STRING, kCmdViewThumbnails, L"Page &Thumbnails");
  AppendMenuW(view, MF_STRING, kCmdViewBookmarks, L"&Bookmarks");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kCmdViewFullScreen,
              L"F&ull Screen Mode\tCtrl+L");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

  HMENU nav = CreatePopupMenu();
  AppendMenuW(nav, MF_STRING, kCmdPageFirst, L"&First Page\tCtrl+Home");
  AppendMenuW(nav, MF_STRING, kCmdPagePrev, L"&Previous Page");
  AppendMenuW(nav, MF_STRING, kCmdPageNext, L"&Next Page");
  AppendMenuW(nav, MF_STRING, kCmdPageLast, L"&Last Page\tCtrl+End");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(nav),
              L"&Navigation");

  HMENU tools = CreatePopupMenu();
  AppendMenuW(tools, MF_STRING, kCmdToolHand, L"&Hand Tool\tH");
  AppendMenuW(tools, MF_STRING, kCmdToolSelect, L"&Select Text\tV");
  AppendMenuW(tools, MF_STRING, kCmdToolMarquee, L"&Marquee Zoom\tZ");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");

  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING, kCmdHelpAbout, L"&About Vellum");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");

  return bar;
}

void SyncMenu(FrameData* d) {
  AppState* s = &d->state;
  HMENU m = d->menu;
  auto check = [&](UINT id, bool on) {
    CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
  };
  auto enable = [&](UINT id, bool on) {
    EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
  };

  const PageLayout l = s->layout.layout();
  check(kCmdLayoutSingle, l == PageLayout::kSinglePage);
  check(kCmdLayoutContinuous, l == PageLayout::kContinuous);
  check(kCmdLayoutTwoPage, l == PageLayout::kTwoPage);
  check(kCmdLayoutTwoPageContinuous, l == PageLayout::kTwoPageContinuous);
  check(kCmdLayoutCoverPage, s->layout.cover_page());

  const ZoomMode z = s->layout.zoom_mode();
  check(kCmdZoomActual, z == ZoomMode::kActualSize);
  check(kCmdZoomFitPage, z == ZoomMode::kFitPage);
  check(kCmdZoomFitWidth, z == ZoomMode::kFitWidth);
  check(kCmdZoomFitVisible, z == ZoomMode::kFitVisibleWidth);

  check(kCmdToolHand, s->tool == Tool::kHand);
  check(kCmdToolSelect, s->tool == Tool::kSelectText);
  check(kCmdToolMarquee, s->tool == Tool::kMarqueeZoom);

  check(kCmdViewNavPane, s->nav_visible);
  check(kCmdViewThumbnails, s->panel == NavPanel::kThumbnails);
  check(kCmdViewBookmarks, s->panel == NavPanel::kBookmarks);
  check(kCmdViewFullScreen, s->full_screen);

  const bool has = s->has_doc();
  for (UINT id : {kCmdFileClose, kCmdFileProperties, kCmdEditCopy,
                  kCmdEditSelectAll, kCmdEditFind, kCmdFindNext,
                  kCmdFindPrev, kCmdZoomIn, kCmdZoomOut, kCmdZoomActual,
                  kCmdZoomFitPage, kCmdZoomFitWidth, kCmdZoomFitVisible,
                  kCmdRotateCW, kCmdRotateCCW})
    enable(id, has);
  enable(kCmdPageFirst, has && s->page > 0);
  enable(kCmdPagePrev, has && s->page > 0);
  enable(kCmdPageNext, has && s->page + 1 < s->doc->page_count());
  enable(kCmdPageLast, has && s->page + 1 < s->doc->page_count());
  enable(kCmdEditCopy, has && !s->selection.empty());
}

// --- child layout ----------------------------------------------------------

void LayoutChildren(FrameData* d) {
  RECT rc;
  GetClientRect(d->hwnd, &rc);

  int top = 0;
  if (!d->state.full_screen) {
    const int tb = ToolbarHeight(d->toolbar);
    MoveWindow(d->toolbar, 0, 0, rc.right, tb, TRUE);
    ShowWindow(d->toolbar, SW_SHOW);
    top = tb;
  } else {
    ShowWindow(d->toolbar, SW_HIDE);
  }

  int bottom = rc.bottom;
  if (!d->state.full_screen) {
    SendMessageW(d->status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(d->status, &sr);
    bottom -= (sr.bottom - sr.top);
    ShowWindow(d->status, SW_SHOW);
  } else {
    ShowWindow(d->status, SW_HIDE);
  }

  if (d->find_visible) {
    const int fh = kFindHeight * static_cast<int>(GetDpiForWindow(d->hwnd)) / 96;
    MoveWindow(d->find, 0, bottom - fh, rc.right, fh, TRUE);
    ShowWindow(d->find, SW_SHOW);
    bottom -= fh;
  } else {
    ShowWindow(d->find, SW_HIDE);
  }

  int left = 0;
  const bool nav = d->state.nav_visible && !d->state.full_screen;
  if (nav) {
    const int w = std::clamp<int>(d->state.nav_width, kMinNavWidth,
                                  std::max<int>(kMinNavWidth, rc.right / 2));
    d->state.nav_width = w;
    MoveWindow(d->nav, 0, top, w, bottom - top, TRUE);
    ShowWindow(d->nav, SW_SHOW);
    left = w + kSplitterWidth;
  } else {
    ShowWindow(d->nav, SW_HIDE);
  }

  MoveWindow(d->view, left, top, std::max<int>(0, rc.right - left),
             std::max<int>(0, bottom - top), TRUE);
}

RECT SplitterRect(FrameData* d) {
  RECT rc;
  GetClientRect(d->hwnd, &rc);
  RECT nv;
  GetWindowRect(d->nav, &nv);
  MapWindowPoints(nullptr, d->hwnd, reinterpret_cast<POINT*>(&nv), 2);
  return RECT{nv.right, nv.top, nv.right + kSplitterWidth, nv.bottom};
}

// --- status bar ------------------------------------------------------------

void SyncStatus(FrameData* d) {
  AppState* s = &d->state;
  if (!s->has_doc()) {
    SendMessageW(d->status, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(L"Ready"));
    SendMessageW(d->status, SB_SETTEXTW, 1,
                 reinterpret_cast<LPARAM>(L""));
    SendMessageW(d->status, SB_SETTEXTW, 2,
                 reinterpret_cast<LPARAM>(L""));
    return;
  }

  const std::wstring page = L"Page " + std::to_wstring(s->page + 1) + L" of " +
                            std::to_wstring(s->doc->page_count());
  SendMessageW(d->status, SB_SETTEXTW, 0,
               reinterpret_cast<LPARAM>(page.c_str()));

  const std::wstring zoom = FormatZoom(s->current.scale / s->dpi_scale);
  SendMessageW(d->status, SB_SETTEXTW, 1,
               reinterpret_cast<LPARAM>(zoom.c_str()));

  const PageGeometry g = s->doc->geometry(s->page);
  wchar_t buf[128];
  swprintf(buf, 128, L"%s   %.2f × %.2f in",
           Utf8ToWide(s->doc->format_name()).c_str(), g.width_pt / 72.0,
           g.height_pt / 72.0);
  SendMessageW(d->status, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(buf));
}

void SyncAll(FrameData* d) {
  ToolbarSyncState(d->toolbar);
  NavPaneSyncPage(d->nav);
  SyncStatus(d);
  SyncMenu(d);
  InvalidateRect(d->nav, nullptr, FALSE);
}

void SyncTitle(FrameData* d) {
  std::wstring title;
  if (d->state.has_doc()) {
    title = d->state.file_name;
    const std::string dt = d->state.doc->title();
    if (!dt.empty()) title = Utf8ToWide(dt) + L" — " + title;
    title += L" - ";
  }
  title += kAppName;
  SetWindowTextW(d->hwnd, title.c_str());
}

// --- document open/close ---------------------------------------------------

void CloseDocument(FrameData* d) {
  d->state.cache->SetDocument(nullptr);
  d->state.doc.reset();
  d->state.layout.set_document(nullptr);
  d->state.path.clear();
  d->state.file_name.clear();
  d->state.page = 0;
  d->state.selection.clear();
  d->state.hits.clear();
  d->state.hit_index = -1;
  d->state.ResetTextCache();
  PageViewDocumentChanged(d->view);
  NavPaneDocumentChanged(d->nav);
  SyncTitle(d);
  SyncAll(d);
}

void OpenPath(FrameData* d, const std::wstring& path) {
  std::shared_ptr<Document> doc;
  try {
    doc = OpenDocument(path);
  } catch (const std::exception& e) {
    const std::wstring msg =
        L"Could not open this file.\n\n" + Utf8ToWide(e.what());
    MessageBoxW(d->hwnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    return;
  }

  CloseDocument(d);

  d->state.doc = std::move(doc);
  d->state.path = path;
  const size_t slash = path.find_last_of(L"\\/");
  d->state.file_name =
      (slash == std::wstring::npos) ? path : path.substr(slash + 1);

  d->state.layout.set_document(d->state.doc.get());
  d->state.cache->SetDocument(d->state.doc);
  d->state.text_cache.assign(
      static_cast<size_t>(d->state.doc->page_count()), {});
  d->state.text_loaded.assign(
      static_cast<size_t>(d->state.doc->page_count()), false);

  PageViewDocumentChanged(d->view);
  NavPaneDocumentChanged(d->nav);
  SyncTitle(d);
  SyncAll(d);
}

void ShowOpenDialog(FrameData* d) {
  wchar_t file[MAX_PATH * 4]{};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = d->hwnd;
  ofn.lpstrFilter =
      L"All supported documents\0*.pdf;*.djvu;*.djv\0"
      L"PDF documents (*.pdf)\0*.pdf\0"
      L"DjVu documents (*.djvu;*.djv)\0*.djvu;*.djv\0"
      L"All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH * 4;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (GetOpenFileNameW(&ofn)) OpenPath(d, file);
}

void ShowProperties(FrameData* d) {
  AppState* s = &d->state;
  if (!s->has_doc()) return;
  const PageGeometry g = s->doc->geometry(s->page);
  std::wstring text;
  text += L"File: " + s->file_name + L"\n";
  text += L"Format: " + Utf8ToWide(s->doc->format_name()) + L"\n";
  const std::string t = s->doc->title();
  if (!t.empty()) text += L"Title: " + Utf8ToWide(t) + L"\n";
  text += L"Pages: " + std::to_wstring(s->doc->page_count()) + L"\n";
  wchar_t buf[128];
  swprintf(buf, 128, L"Page size: %.2f × %.2f in  (%.0f × %.0f pt)\n",
           g.width_pt / 72.0, g.height_pt / 72.0, g.width_pt, g.height_pt);
  text += buf;
  const size_t mb = s->cache->bytes() / (1024 * 1024);
  text += L"Rendered pages cached: " + std::to_wstring(mb) + L" MB\n";
  text += L"Outline: ";
  text += s->doc->outline().empty() ? L"none" : L"present";
  MessageBoxW(d->hwnd, text.c_str(), L"Document Properties",
              MB_OK | MB_ICONINFORMATION);
}

// --- search ----------------------------------------------------------------

std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

// Builds the page's text as one string plus a map from character offset
// back to run index, so a match can be turned into highlight rectangles.
void FlattenPage(AppState* s, int page, std::wstring* text,
                 std::vector<int>* run_of_char) {
  const auto& runs = s->TextFor(page);
  for (size_t i = 0; i < runs.size(); ++i) {
    const std::wstring w = Utf8ToWide(runs[i].text);
    for (size_t c = 0; c < w.size(); ++c) {
      text->push_back(w[c]);
      run_of_char->push_back(static_cast<int>(i));
    }
    // Runs are words; a separator keeps them from merging into one token.
    text->push_back(L' ');
    run_of_char->push_back(static_cast<int>(i));
  }
}

void RunSearch(FrameData* d) {
  AppState* s = &d->state;
  s->hits.clear();
  s->hit_index = -1;
  if (!s->has_doc() || s->find_text.empty()) return;

  const std::wstring needle =
      s->find_case ? s->find_text : Lower(s->find_text);

  for (int p = 0; p < s->doc->page_count(); ++p) {
    std::wstring text;
    std::vector<int> run_of_char;
    FlattenPage(s, p, &text, &run_of_char);
    if (text.empty()) continue;
    const std::wstring hay = s->find_case ? text : Lower(text);

    size_t at = 0;
    while ((at = hay.find(needle, at)) != std::wstring::npos) {
      SearchHit h;
      h.page = p;
      h.first_run = run_of_char[at];
      const size_t end = std::min(at + needle.size() - 1, run_of_char.size() - 1);
      h.last_run = run_of_char[end];
      s->hits.push_back(h);
      at += std::max<size_t>(1, needle.size());
      // A pathological document should not hang the UI.
      if (s->hits.size() > 20000) return;
    }
  }
}

void GoToHit(FrameData* d, int delta) {
  AppState* s = &d->state;
  if (s->hits.empty()) return;
  if (s->hit_index < 0) {
    // Start from the page being read rather than the top of the document.
    s->hit_index = 0;
    for (size_t i = 0; i < s->hits.size(); ++i) {
      if (s->hits[i].page >= s->page) {
        s->hit_index = static_cast<int>(i);
        break;
      }
    }
  } else {
    const int n = static_cast<int>(s->hits.size());
    s->hit_index = (s->hit_index + delta % n + n) % n;
  }
  const SearchHit& h = s->hits[s->hit_index];
  s->page = h.page;
  s->selection.page = h.page;
  s->selection.first_run = h.first_run;
  s->selection.last_run = h.last_run;
  PageViewEnsureHitVisible(d->view, h);
  SyncAll(d);
  InvalidateRect(d->find, nullptr, FALSE);
}

void ShowFindBar(FrameData* d, bool show) {
  d->find_visible = show;
  LayoutChildren(d);
  if (show) {
    FindBarFocus(d->find);
  } else {
    d->state.hits.clear();
    d->state.hit_index = -1;
    d->state.find_text.clear();
    SetFocus(d->view);
    InvalidateRect(d->view, nullptr, FALSE);
  }
}

// --- clipboard -------------------------------------------------------------

void CopySelection(FrameData* d) {
  AppState* s = &d->state;
  if (s->selection.empty()) return;
  const auto& runs = s->TextFor(s->selection.page);
  std::wstring text;
  for (int i = s->selection.first_run;
       i <= s->selection.last_run && i < static_cast<int>(runs.size()); ++i) {
    if (!text.empty()) text += L' ';
    text += Utf8ToWide(runs[i].text);
  }
  if (text.empty()) return;

  if (!OpenClipboard(d->hwnd)) return;
  EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (mem) {
    void* p = GlobalLock(mem);
    memcpy(p, text.c_str(), bytes);
    GlobalUnlock(mem);
    SetClipboardData(CF_UNICODETEXT, mem);
  }
  CloseClipboard();
}

// --- commands --------------------------------------------------------------

void SetZoomMode(FrameData* d, ZoomMode mode) {
  d->state.layout.set_zoom_mode(mode);
  PageViewRelayout(d->view);
  SyncAll(d);
}

void StepZoom(FrameData* d, bool in) {
  AppState* s = &d->state;
  // Acrobat's zoom ladder, so stepping lands on round numbers.
  static const double kStops[] = {0.10, 0.25, 0.50, 0.75, 1.00, 1.25,
                                  1.50, 2.00, 4.00, 8.00, 16.0, 32.0};
  const double now = s->current.scale / s->dpi_scale;
  double target = now;
  if (in) {
    for (double v : kStops)
      if (v > now * 1.001) { target = v; break; }
    if (target == now) target = now * 1.25;
  } else {
    for (int i = static_cast<int>(sizeof(kStops) / sizeof(*kStops)) - 1;
         i >= 0; --i)
      if (kStops[i] < now * 0.999) { target = kStops[i]; break; }
    if (target == now) target = now / 1.25;
  }
  s->layout.set_zoom_mode(ZoomMode::kCustom);
  s->layout.set_zoom(target * s->dpi_scale);
  PageViewRelayout(d->view);
  SyncAll(d);
}

void GoToPage(FrameData* d, int page) {
  AppState* s = &d->state;
  if (!s->has_doc()) return;
  page = std::clamp(page, 0, s->doc->page_count() - 1);
  s->page = page;
  PageViewScrollToPage(d->view, page);
  SyncAll(d);
}

void ToggleFullScreen(FrameData* d) {
  AppState* s = &d->state;
  s->full_screen = !s->full_screen;
  if (s->full_screen) {
    d->saved_placement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(d->hwnd, &d->saved_placement);
    SetMenu(d->hwnd, nullptr);
    SetWindowLongPtrW(d->hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(d->hwnd, MONITOR_DEFAULTTONEAREST),
                    &mi);
    SetWindowPos(d->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
  } else {
    SetMenu(d->hwnd, d->menu);
    SetWindowLongPtrW(d->hwnd, GWL_STYLE,
                      WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
    SetWindowPlacement(d->hwnd, &d->saved_placement);
    SetWindowPos(d->hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  LayoutChildren(d);
  SyncAll(d);
}

void OnCommand(FrameData* d, UINT cmd, LPARAM param) {
  AppState* s = &d->state;

  switch (cmd) {
    case kCmdNone:
      // A view reporting that it changed something the chrome shows.
      SyncAll(d);
      return;

    case kCmdFileOpen: ShowOpenDialog(d); return;
    case kCmdFileClose: CloseDocument(d); return;
    case kCmdFileProperties: ShowProperties(d); return;
    case kCmdFileExit: PostMessageW(d->hwnd, WM_CLOSE, 0, 0); return;

    case kCmdEditCopy: CopySelection(d); return;
    case kCmdEditSelectAll:
      if (s->has_doc()) {
        const auto& runs = s->TextFor(s->page);
        if (!runs.empty()) {
          s->selection.page = s->page;
          s->selection.first_run = 0;
          s->selection.last_run = static_cast<int>(runs.size()) - 1;
          InvalidateRect(d->view, nullptr, FALSE);
          SyncMenu(d);
        }
      }
      return;

    case kCmdEditFind:
      if (!s->has_doc()) return;
      if (param == 1) {
        // Typed into the find box: re-run and jump to the first match.
        RunSearch(d);
        s->hit_index = -1;
        if (!s->hits.empty()) GoToHit(d, 0);
        InvalidateRect(d->find, nullptr, FALSE);
        InvalidateRect(d->view, nullptr, FALSE);
        return;
      }
      ShowFindBar(d, true);
      return;

    case kCmdFindClose:
      // Escape routes here: close the find bar if it is open, otherwise
      // leave full screen.
      if (d->find_visible)
        ShowFindBar(d, false);
      else if (s->full_screen)
        ToggleFullScreen(d);
      return;

    case kCmdFindNext:
    case kCmdFindPrev: {
      if (!s->has_doc()) return;
      if (!d->find_visible) {
        ShowFindBar(d, true);
        return;
      }
      if (s->hits.empty()) RunSearch(d);
      GoToHit(d, cmd == kCmdFindNext ? 1 : -1);
      return;
    }

    case kCmdZoomIn: StepZoom(d, true); return;
    case kCmdZoomOut: StepZoom(d, false); return;
    case kCmdZoomActual: SetZoomMode(d, ZoomMode::kActualSize); return;
    case kCmdZoomFitPage: SetZoomMode(d, ZoomMode::kFitPage); return;
    case kCmdZoomFitWidth: SetZoomMode(d, ZoomMode::kFitWidth); return;
    case kCmdZoomFitVisible:
      SetZoomMode(d, ZoomMode::kFitVisibleWidth);
      return;
    case kCmdZoomFitHeight: SetZoomMode(d, ZoomMode::kFitHeight); return;

    case kCmdZoomTo: {
      const int percent = static_cast<int>(param);
      if (percent <= 0) return;
      s->layout.set_zoom_mode(ZoomMode::kCustom);
      s->layout.set_zoom(percent / 100.0 * s->dpi_scale);
      PageViewRelayout(d->view);
      SyncAll(d);
      return;
    }

    case kCmdRotateCW:
    case kCmdRotateCCW:
      s->layout.set_rotation(s->layout.rotation() +
                             (cmd == kCmdRotateCW ? 90 : -90));
      s->cache->Clear();
      PageViewRelayout(d->view);
      SyncAll(d);
      return;

    case kCmdLayoutSingle:
    case kCmdLayoutContinuous:
    case kCmdLayoutTwoPage:
    case kCmdLayoutTwoPageContinuous: {
      PageLayout l = PageLayout::kContinuous;
      if (cmd == kCmdLayoutSingle) l = PageLayout::kSinglePage;
      else if (cmd == kCmdLayoutTwoPage) l = PageLayout::kTwoPage;
      else if (cmd == kCmdLayoutTwoPageContinuous)
        l = PageLayout::kTwoPageContinuous;
      s->layout.set_layout(l);
      PageViewScrollToPage(d->view, s->page);
      SyncAll(d);
      return;
    }

    case kCmdLayoutCoverPage:
      s->layout.set_cover_page(!s->layout.cover_page());
      PageViewScrollToPage(d->view, s->page);
      SyncAll(d);
      return;

    case kCmdPageFirst: GoToPage(d, 0); return;
    case kCmdPagePrev: GoToPage(d, s->page - 1); return;
    case kCmdPageNext: GoToPage(d, s->page + 1); return;
    case kCmdPageLast:
      if (s->has_doc()) GoToPage(d, s->doc->page_count() - 1);
      return;
    case kCmdPageGoto:
      if (param > 0) GoToPage(d, static_cast<int>(param) - 1);
      return;

    case kCmdToolHand: s->tool = Tool::kHand; SyncAll(d); return;
    case kCmdToolSelect: s->tool = Tool::kSelectText; SyncAll(d); return;
    case kCmdToolMarquee: s->tool = Tool::kMarqueeZoom; SyncAll(d); return;

    case kCmdViewNavPane:
      s->nav_visible = !s->nav_visible;
      LayoutChildren(d);
      SyncAll(d);
      return;
    case kCmdViewThumbnails:
      s->panel = NavPanel::kThumbnails;
      s->nav_visible = true;
      LayoutChildren(d);
      NavPaneDocumentChanged(d->nav);
      SyncAll(d);
      return;
    case kCmdViewBookmarks:
      s->panel = NavPanel::kBookmarks;
      s->nav_visible = true;
      LayoutChildren(d);
      NavPaneDocumentChanged(d->nav);
      SyncAll(d);
      return;
    case kCmdViewFullScreen: ToggleFullScreen(d); return;

    case kCmdHelpAbout:
      MessageBoxW(
          d->hwnd,
          L"Vellum — a PDF and DjVu viewer\n\n"
          L"PDF rendering: PDFium\n"
          L"DjVu: native decoder (IFF, ZP, BZZ, JB2, IW44)\n\n"
          L"Viewer behaviour modelled on Adobe Acrobat's.",
          L"About Vellum", MB_OK | MB_ICONINFORMATION);
      return;
  }
}

// Keyboard shortcuts that are easier to express directly than through an
// accelerator table, because several depend on modifier combinations.
bool HandleKey(FrameData* d, WPARAM key) {
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

  if (ctrl && shift) {
    switch (key) {
      case VK_OEM_PLUS:
      case VK_ADD:
        OnCommand(d, kCmdRotateCW, 0);
        return true;
      case VK_OEM_MINUS:
      case VK_SUBTRACT:
        OnCommand(d, kCmdRotateCCW, 0);
        return true;
    }
  }

  if (ctrl) {
    switch (key) {
      case 'O': OnCommand(d, kCmdFileOpen, 0); return true;
      case 'W': OnCommand(d, kCmdFileClose, 0); return true;
      case 'D': OnCommand(d, kCmdFileProperties, 0); return true;
      case 'C': OnCommand(d, kCmdEditCopy, 0); return true;
      case 'A': OnCommand(d, kCmdEditSelectAll, 0); return true;
      case 'F': OnCommand(d, kCmdEditFind, 0); return true;
      case 'L': OnCommand(d, kCmdViewFullScreen, 0); return true;
      case '0': OnCommand(d, kCmdZoomFitPage, 0); return true;
      case '1': OnCommand(d, kCmdZoomActual, 0); return true;
      case '2': OnCommand(d, kCmdZoomFitWidth, 0); return true;
      case '3': OnCommand(d, kCmdZoomFitVisible, 0); return true;
      case VK_OEM_PLUS:
      case VK_ADD:
        OnCommand(d, kCmdZoomIn, 0);
        return true;
      case VK_OEM_MINUS:
      case VK_SUBTRACT:
        OnCommand(d, kCmdZoomOut, 0);
        return true;
    }
    return false;
  }

  switch (key) {
    case VK_F3:
      OnCommand(d, shift ? kCmdFindPrev : kCmdFindNext, 0);
      return true;
    case VK_F4:
      OnCommand(d, kCmdViewNavPane, 0);
      return true;
    case VK_ESCAPE:
      if (d->find_visible) {
        OnCommand(d, kCmdFindClose, 0);
        return true;
      }
      if (d->state.full_screen) {
        OnCommand(d, kCmdViewFullScreen, 0);
        return true;
      }
      return false;
    case 'H': OnCommand(d, kCmdToolHand, 0); return true;
    case 'V': OnCommand(d, kCmdToolSelect, 0); return true;
    case 'Z': OnCommand(d, kCmdToolMarquee, 0); return true;
  }
  return false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  FrameData* d = Data(hwnd);

  switch (msg) {
    case WM_NCCREATE: {
      auto* data = new FrameData();
      data->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(data));
      return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_CREATE: {
      d->state.dpi_scale = GetDpiForWindow(hwnd) / 96.0;
      d->state.nav_width =
          static_cast<int>(196 * d->state.dpi_scale);
      d->state.layout.set_dpi_scale(d->state.dpi_scale);
      d->state.cache = std::make_unique<RenderCache>([hwnd] {
        // Called on the render thread: only wake the UI thread.
        PostMessageW(hwnd, kMsgPageReady, 0, 0);
      });

      d->menu = BuildMenu();
      SetMenu(hwnd, d->menu);

      d->toolbar = CreateToolbarWindow(hwnd, &d->state);
      d->nav = CreateNavPaneWindow(hwnd, &d->state);
      d->view = CreatePageViewWindow(hwnd, &d->state);
      d->find = CreateFindBarWindow(hwnd, &d->state);

      d->status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                  WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0,
                                  0, 0, 0, hwnd, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
      int parts[3] = {160, 240, -1};
      SendMessageW(d->status, SB_SETPARTS, 3,
                   reinterpret_cast<LPARAM>(parts));

      DragAcceptFiles(hwnd, TRUE);
      LayoutChildren(d);
      SyncTitle(d);
      SyncAll(d);
      SetFocus(d->view);
      return 0;
    }

    case WM_DESTROY:
      // Stop the render thread before the document it is using goes away.
      d->state.cache.reset();
      delete d;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      PostQuitMessage(0);
      return 0;

    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) LayoutChildren(d);
      return 0;

    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize.x = 560;
      mm->ptMinTrackSize.y = 360;
      return 0;
    }

    case WM_SETFOCUS:
      SetFocus(d->view);
      return 0;

    case kMsgPageReady:
      // A page finished rendering: repaint whatever shows it.
      InvalidateRect(d->view, nullptr, FALSE);
      if (d->state.nav_visible) InvalidateRect(d->nav, nullptr, FALSE);
      return 0;

    case kMsgCommand:
      OnCommand(d, static_cast<UINT>(wp), lp);
      return 0;

    case WM_COMMAND:
      if (HIWORD(wp) == 0 || HIWORD(wp) == 1) {  // menu or accelerator
        OnCommand(d, LOWORD(wp), 0);
        return 0;
      }
      break;

    case WM_INITMENUPOPUP:
      SyncMenu(d);
      return 0;

    case WM_KEYDOWN:
      if (HandleKey(d, wp)) return 0;
      break;

    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wp);
      wchar_t path[MAX_PATH * 4]{};
      if (DragQueryFileW(drop, 0, path, MAX_PATH * 4)) OpenPath(d, path);
      DragFinish(drop);
      SetForegroundWindow(hwnd);
      return 0;
    }

    case WM_SETCURSOR: {
      POINT pt;
      GetCursorPos(&pt);
      ScreenToClient(hwnd, &pt);
      if (d->state.nav_visible && !d->state.full_screen) {
        RECT sp = SplitterRect(d);
        if (PtInRect(&sp, pt)) {
          SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
          return TRUE;
        }
      }
      break;
    }

    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (d->state.nav_visible && !d->state.full_screen) {
        RECT sp = SplitterRect(d);
        if (PtInRect(&sp, pt)) {
          d->dragging_splitter = true;
          SetCapture(hwnd);
          return 0;
        }
      }
      break;
    }

    case WM_MOUSEMOVE:
      if (d->dragging_splitter) {
        d->state.nav_width = GET_X_LPARAM(lp);
        LayoutChildren(d);
        return 0;
      }
      break;

    case WM_LBUTTONUP:
      if (d->dragging_splitter) {
        d->dragging_splitter = false;
        ReleaseCapture();
        return 0;
      }
      break;

    case WM_DPICHANGED: {
      d->state.dpi_scale = HIWORD(wp) / 96.0;
      d->state.layout.set_dpi_scale(d->state.dpi_scale);
      const RECT* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      d->state.cache->Clear();
      LayoutChildren(d);
      PageViewRelayout(d->view);
      SyncAll(d);
      return 0;
    }

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreateMainWindow(const std::wstring& open_path) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  RegisterClassExW(&wc);

  // Open at a readable fraction of the work area rather than a fixed pixel
  // size, so the window is sensible on both a laptop panel and a 4K display.
  RECT work{0, 0, 1280, 900};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int work_w = work.right - work.left;
  const int work_h = work.bottom - work.top;
  const int win_w = std::clamp<int>(work_w * 3 / 4, 900, 1800);
  const int win_h = std::clamp<int>(work_h * 7 / 8, 640, 1300);

  HWND hwnd = CreateWindowExW(
      WS_EX_ACCEPTFILES, kClassName, kAppName,
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      work.left + (work_w - win_w) / 2, work.top + (work_h - win_h) / 2,
      win_w, win_h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!hwnd) return nullptr;

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  if (!open_path.empty()) {
    FrameData* d = Data(hwnd);
    if (d) OpenPath(d, open_path);
  }
  return hwnd;
}

}  // namespace vellum::ui
