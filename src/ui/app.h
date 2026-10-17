// Shared viewer state and the command vocabulary the UI is built from.
//
// The command list mirrors the one Acrobat's viewer exposes, because the
// goal is a viewer that behaves the way people already expect: the same
// zoom and fit modes, the same page layouts, the same tools, the same
// keyboard shortcuts.
#pragma once

// The full Windows headers: the UI uses common controls and common
// dialogs, which WIN32_LEAN_AND_MEAN would exclude.
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "doc/document.h"
#include "view/layout.h"
#include "view/render_cache.h"

namespace vellum::ui {

// Which pointer tool is active.
enum class Tool {
  kHand,        // drag to pan
  kSelectText,  // drag to select text
  kMarqueeZoom, // drag a rectangle to zoom to it
};

// Which panel the navigation pane is showing.
enum class NavPanel {
  kThumbnails,
  kBookmarks,
};

// Menu and toolbar command identifiers.
enum Command : UINT {
  kCmdNone = 0,

  kCmdFileOpen = 100,
  kCmdFileClose,
  kCmdFileProperties,
  kCmdFileExit,
  kCmdFileOpenHere,  // open into the current tab instead of a new one

  kCmdEditCopy = 200,
  kCmdEditSelectAll,
  kCmdEditFind,
  kCmdFindNext,
  kCmdFindPrev,
  kCmdFindClose,

  kCmdZoomIn = 300,
  kCmdZoomOut,
  kCmdZoomActual,
  kCmdZoomFitPage,
  kCmdZoomFitWidth,
  kCmdZoomFitVisible,
  kCmdZoomFitHeight,
  kCmdZoomTo,  // from the toolbar's zoom box

  kCmdRotateCW = 400,
  kCmdRotateCCW,

  kCmdLayoutSingle = 500,
  kCmdLayoutContinuous,
  kCmdLayoutTwoPage,
  kCmdLayoutTwoPageContinuous,
  kCmdLayoutCoverPage,

  kCmdPageFirst = 600,
  kCmdPagePrev,
  kCmdPageNext,
  kCmdPageLast,
  kCmdPageGoto,  // from the toolbar's page box

  kCmdToolHand = 700,
  kCmdToolSelect,
  kCmdToolMarquee,

  kCmdViewNavPane = 800,
  kCmdViewThumbnails,
  kCmdViewBookmarks,
  kCmdViewFullScreen,

  kCmdHelpAbout = 900,

  // Tabs. kCmdTabSelect carries the tab index in lParam; kCmdTabCloseAt
  // carries that index plus one, so zero can still mean "the active tab".
  kCmdTabClose = 1000,
  kCmdTabNext,
  kCmdTabPrev,
  kCmdTabSelect,
  kCmdTabCloseAt,
};

// Private window messages.
enum {
  // Posted by a render worker when a page becomes available.
  kMsgPageReady = WM_APP + 1,
  // Posted by a child window asking the frame to run a command.
  kMsgCommand = WM_APP + 2,
};

// A hit inside a page: which page, and where within it in normalised
// coordinates. Keeping selection in normalised space means it survives
// zooming and rotation.
struct PageHit {
  int page = -1;
  float x = 0;
  float y = 0;
  bool valid() const { return page >= 0; }
};

// A span of selected text, as a half-open range of runs on one page.
struct Selection {
  int page = -1;
  int first_run = 0;
  int last_run = -1;  // inclusive
  bool empty() const { return page < 0 || last_run < first_run; }
  void clear() { page = -1; first_run = 0; last_run = -1; }
};

// One search result.
struct SearchHit {
  int page = 0;
  int first_run = 0;
  int last_run = 0;
};

// Everything belonging to one open document: what it is, where the reader
// is in it, and how it is being shown. One of these per tab, so switching
// tabs restores the page, zoom, layout, selection and search exactly as
// they were left.
struct DocumentTab {
  std::shared_ptr<Document> doc;
  std::wstring path;
  std::wstring file_name;

  LayoutEngine layout;
  std::unique_ptr<RenderCache> cache;
  Layout current;  // last built layout

  int page = 0;        // current page, 0-based
  POINT scroll{0, 0};  // document-space scroll origin

  Selection selection;
  std::vector<SearchHit> hits;
  int hit_index = -1;
  std::wstring find_text;
  bool find_case = false;

  // Text runs are cached per page: selection, search and copy all need
  // them, and extracting them is not free.
  std::vector<std::vector<TextRun>> text_cache;
  std::vector<bool> text_loaded;

  bool has_doc() const { return doc != nullptr; }
  const std::vector<TextRun>& TextFor(int index);
  void ResetTextCache();

  // What this tab shows on its label.
  std::wstring Label() const;
};

// State shared by every tab: the window chrome, the active tool, and the
// list of open documents.
struct AppState {
  // There is always at least one tab. An empty one is the no-document
  // state, which keeps every window below free of null checks.
  std::vector<std::unique_ptr<DocumentTab>> tabs;
  int active = 0;

  DocumentTab& t() { return *tabs[active]; }
  const DocumentTab& t() const { return *tabs[active]; }
  bool has_doc() const { return t().has_doc(); }

  // Chrome is shared across tabs, the way it is in a browser.
  Tool tool = Tool::kHand;
  NavPanel panel = NavPanel::kThumbnails;
  bool nav_visible = true;
  int nav_width = 196;
  bool full_screen = false;
  double dpi_scale = 1.0;

  // Handed to every tab's render cache so a finished page wakes the frame.
  RenderCache::Notify notify;

  const std::vector<TextRun>& TextFor(int index) { return t().TextFor(index); }
};

// Formats a zoom ratio the way the toolbar box shows it.
std::wstring FormatZoom(double zoom);

// Converts UTF-8 to UTF-16 and back, for text that crosses the Win32 API.
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// Window factories. Each returns the created child window.
HWND CreateToolbarWindow(HWND parent, AppState* state);
HWND CreateTabBarWindow(HWND parent, AppState* state);
HWND CreateNavPaneWindow(HWND parent, AppState* state);
HWND CreatePageViewWindow(HWND parent, AppState* state);
HWND CreateFindBarWindow(HWND parent, AppState* state);

// Child-window notifications the frame reacts to.
void ToolbarSyncState(HWND toolbar);
int ToolbarHeight(HWND toolbar);
void TabBarSync(HWND bar);
int TabBarHeight(HWND bar);
void NavPaneDocumentChanged(HWND nav);
void NavPaneSyncPage(HWND nav);
void PageViewDocumentChanged(HWND view);
void PageViewRelayout(HWND view);
void PageViewScrollToPage(HWND view, int page);
void PageViewEnsureHitVisible(HWND view, const SearchHit& hit);
void FindBarFocus(HWND bar);

}  // namespace vellum::ui
