// Entry point and the small helpers shared across the UI.

#include "ui/app.h"  // brings in <windows.h>; must precede the headers below

#include <commctrl.h>
#include <shellapi.h>

#include <cmath>
#include <vector>
#include <iterator>
#include <string>

namespace vellum::ui {

HWND CreateMainWindow(const std::vector<std::wstring>& open_paths);

// Shortcuts that carry a modifier are safe to dispatch globally: they do
// not collide with typing in the page or zoom boxes. Plain-key shortcuts
// (H, V, Z, Escape) are handled by the page view instead, which only has
// focus when the user is not typing.
HACCEL BuildAccelerators() {
  const ACCEL table[] = {
      {FVIRTKEY | FCONTROL, 'O', kCmdFileOpen},
      {FVIRTKEY | FCONTROL, 'W', kCmdTabClose},
      {FVIRTKEY | FCONTROL, 'D', kCmdFileProperties},
      {FVIRTKEY | FCONTROL, 'C', kCmdEditCopy},
      {FVIRTKEY | FCONTROL, 'A', kCmdEditSelectAll},
      {FVIRTKEY | FCONTROL, 'F', kCmdEditFind},
      {FVIRTKEY, VK_F3, kCmdFindNext},
      {FVIRTKEY | FSHIFT, VK_F3, kCmdFindPrev},
      {FVIRTKEY, VK_F4, kCmdViewNavPane},
      {FVIRTKEY | FCONTROL, 'L', kCmdViewFullScreen},
      {FVIRTKEY | FCONTROL, '0', kCmdZoomFitPage},
      {FVIRTKEY | FCONTROL, '1', kCmdZoomActual},
      {FVIRTKEY | FCONTROL, '2', kCmdZoomFitWidth},
      {FVIRTKEY | FCONTROL, '3', kCmdZoomFitVisible},
      {FVIRTKEY | FCONTROL, VK_OEM_PLUS, kCmdZoomIn},
      {FVIRTKEY | FCONTROL, VK_ADD, kCmdZoomIn},
      {FVIRTKEY | FCONTROL, VK_OEM_MINUS, kCmdZoomOut},
      {FVIRTKEY | FCONTROL, VK_SUBTRACT, kCmdZoomOut},
      {FVIRTKEY | FCONTROL | FSHIFT, VK_OEM_PLUS, kCmdRotateCW},
      {FVIRTKEY | FCONTROL | FSHIFT, VK_ADD, kCmdRotateCW},
      {FVIRTKEY | FCONTROL | FSHIFT, VK_OEM_MINUS, kCmdRotateCCW},
      {FVIRTKEY | FCONTROL | FSHIFT, VK_SUBTRACT, kCmdRotateCCW},
      {FVIRTKEY | FCONTROL, VK_TAB, kCmdTabNext},
      {FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, kCmdTabPrev},
      {FVIRTKEY | FCONTROL, VK_F4, kCmdTabClose},
      {FVIRTKEY | FCONTROL | FSHIFT, 'O', kCmdFileOpenHere},
      {FVIRTKEY | FCONTROL, VK_HOME, kCmdPageFirst},
      {FVIRTKEY | FCONTROL, VK_END, kCmdPageLast},
  };
  return CreateAcceleratorTableW(const_cast<ACCEL*>(table),
                                 static_cast<int>(std::size(table)));
}

const std::vector<TextRun>& DocumentTab::TextFor(int index) {
  static const std::vector<TextRun> kEmpty;
  if (!doc || index < 0 || index >= static_cast<int>(text_cache.size()))
    return kEmpty;
  if (!text_loaded[index]) {
    // Extracting text is comparatively cheap, but not free, so each page
    // is pulled once and kept for selection, search and copy.
    text_cache[index] = doc->Text(index);
    text_loaded[index] = true;
  }
  return text_cache[index];
}

void DocumentTab::ResetTextCache() {
  text_cache.clear();
  text_loaded.clear();
}

std::wstring DocumentTab::Label() const {
  if (!doc) return L"Untitled";
  // A backend whose pages are separate files names the one being shown,
  // so a tab browsing a folder of pictures tracks the current picture.
  const std::string page_name = doc->page_label(page);
  if (!page_name.empty()) return Utf8ToWide(page_name);
  return file_name.empty() ? L"Untitled" : file_name;
}

std::wstring FormatZoom(double zoom) {
  const int percent = static_cast<int>(std::lround(zoom * 100.0));
  return std::to_wstring(percent) + L"%";
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), n);
  return out;
}

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace vellum::ui

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // Per-monitor DPI so pages stay sharp when the window moves between
  // displays with different scaling.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  INITCOMMONCONTROLSEX icc{sizeof(icc),
                           ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES |
                               ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
  InitCommonControlsEx(&icc);

  // Paths on the command line open straight away, which is what makes
  // "open with" and file associations work. Several arrive when a
  // multiple selection is opened from Explorer; each gets its own tab.
  std::vector<std::wstring> open_paths;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv) {
    for (int i = 1; i < argc; ++i)
      if (argv[i][0] != L'-') open_paths.emplace_back(argv[i]);
    LocalFree(argv);
  }

  HWND main = vellum::ui::CreateMainWindow(open_paths);
  if (!main) return 1;
  HACCEL accel = vellum::ui::BuildAccelerators();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // Accelerators are dispatched against the frame regardless of which
    // child has focus; without this a shortcut pressed while the page view
    // is focused would be swallowed by the child.
    if (!TranslateAcceleratorW(main, accel, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  DestroyAcceleratorTable(accel);
  return 0;
}
