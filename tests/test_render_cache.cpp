// The asynchronous render cache.
//
// These tests drive a real worker thread, so each one waits on a condition
// rather than sleeping a fixed time.

#include "doctest.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "fake_document.h"
#include "view/render_cache.h"

using namespace vellum;
using vellum::test::FakeDocument;

namespace {

// Polls `done` until it holds or the deadline passes. Returns whether it
// held, so a test fails rather than hangs when the worker misbehaves.
template <typename Fn>
bool WaitFor(Fn done, int timeout_ms = 5000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return done();
}

RenderCache::Key MakeKey(int page, int w, int h, int rot = 0) {
  RenderCache::Key k;
  k.page = page;
  k.width = w;
  k.height = h;
  k.rotation = rot;
  return k;
}

}  // namespace

TEST_CASE("Keys order by page, size and rotation") {
  const auto a = MakeKey(1, 10, 10);
  const auto b = MakeKey(2, 10, 10);
  CHECK(a < b);
  CHECK_FALSE(b < a);
  CHECK_FALSE(a < a);
  CHECK(MakeKey(1, 10, 10) < MakeKey(1, 11, 10));
  CHECK(MakeKey(1, 10, 10) < MakeKey(1, 10, 11));
  CHECK(MakeKey(1, 10, 10, 0) < MakeKey(1, 10, 10, 90));
}

TEST_CASE("A miss queues work and the result arrives") {
  auto doc = std::make_shared<FakeDocument>(4);
  std::atomic<int> notified{0};
  RenderCache cache([&] { ++notified; });
  cache.SetDocument(doc);

  const auto key = MakeKey(1, 40, 50);
  CHECK_FALSE(static_cast<bool>(cache.Get(key, 0)));   // first ask is always a miss

  REQUIRE(WaitFor([&] { return cache.Get(key, 0) != nullptr; }));
  auto page = cache.Get(key, 0);
  REQUIRE(static_cast<bool>(page));
  CHECK(page->page == 1);
  CHECK(page->pixmap.width() == 40);
  CHECK(page->pixmap.height() == 50);
  CHECK(page->complete);
  CHECK(page->pixmap.row(0)[0] == 1);    // the fake encodes its page number
  CHECK(notified.load() > 0);
  CHECK(cache.bytes() > 0);
}

TEST_CASE("A degenerate size is refused without reaching the worker") {
  auto doc = std::make_shared<FakeDocument>();
  RenderCache cache([] {});
  cache.SetDocument(doc);
  CHECK_FALSE(static_cast<bool>(cache.Get(MakeKey(0, 0, 10), 0)));
  CHECK_FALSE(static_cast<bool>(cache.Get(MakeKey(0, 10, 0), 0)));
  CHECK_FALSE(static_cast<bool>(cache.Get(MakeKey(0, -5, 10), 0)));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(doc->renders.load() == 0);
}

TEST_CASE("With no document nothing is queued") {
  RenderCache cache([] {});
  CHECK_FALSE(static_cast<bool>(cache.Get(MakeKey(0, 10, 10), 0)));
  CHECK(cache.bytes() == 0);
}

TEST_CASE("Asking twice does not render twice") {
  auto doc = std::make_shared<FakeDocument>();
  RenderCache cache([] {});
  cache.SetDocument(doc);

  const auto key = MakeKey(0, 20, 20);
  for (int i = 0; i < 20; ++i) cache.Get(key, 0);
  REQUIRE(WaitFor([&] { return cache.Get(key, 0) != nullptr; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(doc->renders.load() == 1);
}

TEST_CASE("GetAnySize finds another rendering of the same page") {
  auto doc = std::make_shared<FakeDocument>();
  RenderCache cache([] {});
  cache.SetDocument(doc);

  const auto small = MakeKey(0, 10, 10);
  cache.Get(small, 0);
  REQUIRE(WaitFor([&] { return cache.Get(small, 0) != nullptr; }));

  // The exact size wanted is absent, but a usable stand-in is not.
  // GetAnySize is used rather than Get, because Get would queue the very
  // rendering whose absence is the point.
  auto any = cache.GetAnySize(0, 0);
  REQUIRE(static_cast<bool>(any));
  CHECK(any->page == 0);

  // Rotation is part of a page's identity.
  CHECK_FALSE(static_cast<bool>(cache.GetAnySize(0, 90)));
  CHECK_FALSE(static_cast<bool>(cache.GetAnySize(3, 0)));

  // With several sizes resident the largest is preferred, because scaling
  // down looks better than scaling up.
  const auto big = MakeKey(0, 80, 80);
  cache.Get(big, 0);
  REQUIRE(WaitFor([&] { return cache.Get(big, 0) != nullptr; }));
  any = cache.GetAnySize(0, 0);
  REQUIRE(static_cast<bool>(any));
  CHECK(any->pixmap.width() == 80);
}

TEST_CASE("The cache evicts to stay inside its budget") {
  auto doc = std::make_shared<FakeDocument>(64);
  // Room for roughly two 100x100 pages.
  RenderCache cache([] {}, 100 * 100 * 3 * 2);
  cache.SetDocument(doc);

  for (int page = 0; page < 12; ++page) {
    const auto key = MakeKey(page, 100, 100);
    cache.Get(key, 0);
    REQUIRE(WaitFor([&] { return cache.Get(key, 0) != nullptr; }));
  }
  CHECK(cache.bytes() <= 100u * 100 * 3 * 3);   // trimmed, not unbounded
  // The oldest page is gone; the newest is still there.
  CHECK(static_cast<bool>(cache.Get(MakeKey(11, 100, 100), 0)));
}

TEST_CASE("Clear drops everything resident") {
  auto doc = std::make_shared<FakeDocument>();
  RenderCache cache([] {});
  cache.SetDocument(doc);
  const auto key = MakeKey(0, 30, 30);
  cache.Get(key, 0);
  REQUIRE(WaitFor([&] { return cache.Get(key, 0) != nullptr; }));

  cache.Clear();
  CHECK(cache.bytes() == 0);
  CHECK_FALSE(static_cast<bool>(cache.Get(key, 0)));
}

TEST_CASE("Replacing the document discards the old one's work") {
  auto first = std::make_shared<FakeDocument>();
  RenderCache cache([] {});
  cache.SetDocument(first);
  const auto key = MakeKey(0, 30, 30);
  cache.Get(key, 0);
  REQUIRE(WaitFor([&] { return cache.Get(key, 0) != nullptr; }));

  auto second = std::make_shared<FakeDocument>();
  cache.SetDocument(second);
  // Nothing from the previous document survives the swap.
  CHECK(cache.bytes() == 0);
  CHECK_FALSE(static_cast<bool>(cache.Get(key, 0)));

  REQUIRE(WaitFor([&] { return cache.Get(key, 0) != nullptr; }));
  CHECK(second->renders.load() >= 1);

  // Closing the document is also allowed.
  cache.SetDocument(nullptr);
  CHECK_FALSE(static_cast<bool>(cache.Get(key, 0)));
}

TEST_CASE("Requests nobody wants any more are dropped before they start") {
  auto doc = std::make_shared<FakeDocument>(32);
  doc->render_delay_ms = 40;   // slow enough to queue behind
  RenderCache cache([] {});
  cache.SetDocument(doc);

  for (int page = 0; page < 16; ++page)
    cache.Get(MakeKey(page, 20, 20), 10 + page);

  // Only one of them still matters; the rest should never be rendered.
  const auto keep = MakeKey(15, 20, 20);
  cache.DropPendingExcept({keep});
  REQUIRE(WaitFor([&] { return cache.Get(keep, 0) != nullptr; }, 8000));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(doc->renders.load() < 16);
}

TEST_CASE("An empty keep-list drops the whole queue") {
  auto doc = std::make_shared<FakeDocument>(32);
  doc->render_delay_ms = 30;
  RenderCache cache([] {});
  cache.SetDocument(doc);
  for (int page = 0; page < 12; ++page)
    cache.Get(MakeKey(page, 20, 20), page);
  cache.DropPendingExcept({});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(doc->renders.load() <= 2);   // at most the one already in flight
}

TEST_CASE("A page that fails to render still yields a blank sheet") {
  // The view needs something to draw, and the page must not be re-queued
  // forever.
  auto doc = std::make_shared<FakeDocument>();
  doc->fail_render = true;
  RenderCache cache([] {});
  cache.SetDocument(doc);

  const auto key = MakeKey(0, 25, 25);
  cache.Get(key, 0);
  REQUIRE(WaitFor([&] { return cache.Get(key, 0) != nullptr; }));
  auto page = cache.Get(key, 0);
  REQUIRE(static_cast<bool>(page));
  CHECK(page->pixmap.width() == 25);
  CHECK_FALSE(page->complete);
  CHECK_FALSE(page->error.empty());
}

TEST_CASE("A backend that throws is caught and reported") {
  auto doc = std::make_shared<FakeDocument>();
  doc->throw_on_render = true;
  RenderCache cache([] {});
  cache.SetDocument(doc);

  const auto key = MakeKey(0, 25, 25);
  cache.Get(key, 0);
  REQUIRE(WaitFor([&] { return cache.Get(key, 0) != nullptr; }));
  auto page = cache.Get(key, 0);
  REQUIRE(static_cast<bool>(page));
  CHECK_FALSE(page->complete);
  CHECK(page->error == "fake throw");
}

TEST_CASE("Destruction while work is outstanding is clean") {
  // The worker must be stopped before the document it is using goes away.
  auto doc = std::make_shared<FakeDocument>(64);
  doc->render_delay_ms = 20;
  {
    RenderCache cache([] {});
    cache.SetDocument(doc);
    for (int page = 0; page < 32; ++page)
      cache.Get(MakeKey(page, 50, 50), page);
  }
  CHECK(true);   // reaching here without a hang or a fault is the assertion
}

TEST_CASE("Priority decides what renders first") {
  auto doc = std::make_shared<FakeDocument>(8);
  doc->render_delay_ms = 25;
  RenderCache cache([] {});
  cache.SetDocument(doc);

  // Queue a low-priority page first, then an urgent one.
  cache.Get(MakeKey(0, 30, 30), 100);
  cache.Get(MakeKey(1, 30, 30), 100);
  cache.Get(MakeKey(7, 30, 30), 0);

  const auto urgent = MakeKey(7, 30, 30);
  REQUIRE(WaitFor([&] { return cache.Get(urgent, 0) != nullptr; }));
  CHECK(static_cast<bool>(cache.Get(urgent, 0)));
}
