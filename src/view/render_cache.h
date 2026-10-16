// Asynchronous page rendering with an LRU bitmap cache.
//
// Rendering a page costs tens of milliseconds, far too long to do while
// painting. A worker thread renders on demand and the view paints
// whatever is ready, falling back to a placeholder; when a page arrives
// the cache notifies the window, which repaints. That is the same
// arrangement Acrobat uses, and it is what keeps scrolling responsive on
// a document whose pages cannot all be held in memory.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "doc/document.h"

namespace vellum {

class RenderCache {
 public:
  // Identifies one rendering of one page.
  struct Key {
    int page = 0;
    int width = 0;
    int height = 0;
    int rotation = 0;

    bool operator<(const Key& o) const {
      if (page != o.page) return page < o.page;
      if (width != o.width) return width < o.width;
      if (height != o.height) return height < o.height;
      return rotation < o.rotation;
    }
  };

  using Notify = std::function<void()>;

  // `notify` is called from the worker thread whenever a page becomes
  // available, so it must do nothing but wake the UI thread.
  explicit RenderCache(Notify notify);
  ~RenderCache();

  // Replaces the document and drops everything cached and queued.
  void SetDocument(std::shared_ptr<Document> doc);

  // Returns the rendering if it is ready. Otherwise queues it at
  // `priority` -- lower renders sooner -- and returns null.
  std::shared_ptr<const RenderedPage> Get(const Key& key, int priority);

  // Looks for an already-rendered version of the same page at any size.
  // Scaling one of those is what fills the gap while the right size is
  // still rendering, instead of showing an empty rectangle.
  std::shared_ptr<const RenderedPage> GetAnySize(int page, int rotation);

  // Forgets every queued request that is not in `keep`. Called when the
  // view scrolls or zooms so the worker stops on work nobody wants.
  void DropPendingExcept(const std::vector<Key>& keep);

  void Clear();

  // Approximate resident bytes, for the status bar.
  size_t bytes() const;

 private:
  struct Entry {
    Key key;
    std::shared_ptr<const RenderedPage> page;
    size_t bytes = 0;
  };

  struct Request {
    Key key;
    int priority = 0;
  };

  void WorkerMain();
  void Insert(const Key& key, std::shared_ptr<const RenderedPage> page);
  void TrimLocked();

  // Cap chosen so a full screen of pages at high zoom stays resident
  // without the process growing without bound on a long document.
  static constexpr size_t kMaxBytes = 320u * 1024 * 1024;

  Notify notify_;
  std::shared_ptr<Document> doc_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool quit_ = false;

  // LRU: most recently used at the front.
  std::list<Entry> lru_;
  std::map<Key, std::list<Entry>::iterator> index_;
  size_t bytes_ = 0;

  std::vector<Request> queue_;
  // Generation guards against a worker finishing a render for a document
  // that has since been closed.
  uint64_t generation_ = 0;

  std::thread worker_;
};

}  // namespace vellum
