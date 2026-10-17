// Test runner.
//
// The suite covers everything except the Win32 windows: the format back
// ends, the document model, the layout engine and the render cache. The UI
// needs a running message loop and a desktop, and is exercised by
// tools/screenshot.ps1 instead.

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <cstdio>

#include "fixtures.h"

int main(int argc, char** argv) {
  doctest::Context context;
  context.applyCommandLine(argc, argv);

  if (!vellum::test::HasCorpus()) {
    std::printf(
        "note: the DjVu sample corpus is absent, so the codec tests will\n"
        "      skip. Run tools\\fetch-samples.ps1 to include them.\n\n");
  }

  const int result = context.run();
  if (context.shouldExit()) return result;
  return result;
}
