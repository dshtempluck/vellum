#include "view/render_cache.h"

#include <algorithm>

namespace vellum {

RenderCache::RenderCache(Notify notify) : notify_(std::move(notify)) {
  worker_ = std::thread([this] { WorkerMain(); });
}

RenderCache::~RenderCache() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    quit_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void RenderCache::SetDocument(std::shared_ptr<Document> doc) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    doc_ = std::move(doc);
    lru_.clear();
    index_.clear();
    bytes_ = 0;
    queue_.clear();
    ++generation_;
  }
  cv_.notify_all();
}

void RenderCache::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  lru_.clear();
  index_.clear();
  bytes_ = 0;
  queue_.clear();
  ++generation_;
}

size_t RenderCache::bytes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return bytes_;
}

std::shared_ptr<const RenderedPage> RenderCache::Get(const Key& key,
                                                     int priority) {
  if (key.width <= 0 || key.height <= 0) return nullptr;

  std::unique_lock<std::mutex> lock(mu_);
  auto it = index_.find(key);
  if (it != index_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->page;
  }
  if (!doc_) return nullptr;

  // Already queued? Keep the more urgent priority.
  for (Request& r : queue_) {
    if (!(r.key < key) && !(key < r.key)) {
      r.priority = std::min(r.priority, priority);
      return nullptr;
    }
  }
  queue_.push_back(Request{key, priority});
  lock.unlock();
  cv_.notify_one();
  return nullptr;
}

std::shared_ptr<const RenderedPage> RenderCache::GetAnySize(int page,
                                                            int rotation) {
  std::lock_guard<std::mutex> lock(mu_);
  // Prefer the largest available rendering: scaling down looks better
  // than scaling up.
  std::shared_ptr<const RenderedPage> best;
  long long best_area = 0;
  for (const Entry& e : lru_) {
    if (e.key.page != page || e.key.rotation != rotation) continue;
    const long long area =
        static_cast<long long>(e.key.width) * e.key.height;
    if (area > best_area) {
      best_area = area;
      best = e.page;
    }
  }
  return best;
}

void RenderCache::DropPendingExcept(const std::vector<Key>& keep) {
  std::lock_guard<std::mutex> lock(mu_);
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                              [&](const Request& r) {
                                for (const Key& k : keep)
                                  if (!(k < r.key) && !(r.key < k))
                                    return false;
                                return true;
                              }),
               queue_.end());
}

void RenderCache::Insert(const Key& key,
                         std::shared_ptr<const RenderedPage> page) {
  // Caller holds mu_.
  if (index_.count(key)) return;
  Entry e;
  e.key = key;
  e.bytes = static_cast<size_t>(page->pixmap.width()) *
            page->pixmap.height() * 3;
  e.page = std::move(page);
  bytes_ += e.bytes;
  lru_.push_front(std::move(e));
  index_[key] = lru_.begin();
  TrimLocked();
}

void RenderCache::TrimLocked() {
  while (bytes_ > kMaxBytes && lru_.size() > 1) {
    Entry& back = lru_.back();
    bytes_ -= back.bytes;
    index_.erase(back.key);
    lru_.pop_back();
  }
}

void RenderCache::WorkerMain() {
  while (true) {
    Request req;
    std::shared_ptr<Document> doc;
    uint64_t gen = 0;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
      if (quit_) return;

      // Take the most urgent request; ties go to the one queued first,
      // which keeps a scroll rendering top-down.
      auto best = std::min_element(
          queue_.begin(), queue_.end(),
          [](const Request& a, const Request& b) {
            return a.priority < b.priority;
          });
      req = *best;
      queue_.erase(best);
      doc = doc_;
      gen = generation_;
      if (!doc) continue;

      // It may have been rendered while waiting.
      if (index_.count(req.key)) continue;
    }

    auto page = std::make_shared<RenderedPage>();
    try {
      *page = doc->Render(req.key.page, Size{req.key.width, req.key.height},
                          req.key.rotation);
    } catch (const std::exception& e) {
      page->page = req.key.page;
      page->error = e.what();
      page->complete = false;
    }
    if (page->pixmap.empty()) {
      // Leave a white sheet so the view has something to show and does
      // not re-queue the same failing page forever.
      page->pixmap.Init(req.key.width, req.key.height);
      page->pixmap.Fill(0xff, 0xff, 0xff);
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      if (quit_) return;
      // Discard work for a document that has since been replaced.
      if (gen != generation_) continue;
      Insert(req.key, std::move(page));
    }
    if (notify_) notify_();
  }
}

}  // namespace vellum
