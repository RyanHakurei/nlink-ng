#include "nlink_internal.hpp"

#include <atomic>

namespace nlink {
namespace {

std::atomic<uint64_t> g_total{0};
std::atomic<uint64_t> g_remaining{0};

}  // namespace

void progress_reset(uint64_t total) {
  g_total.store(total, std::memory_order_relaxed);
  g_remaining.store(total, std::memory_order_relaxed);
}

void progress_set_remaining(uint64_t remaining) {
  uint64_t total = g_total.load(std::memory_order_relaxed);
  if (remaining > total) {
    g_total.store(remaining, std::memory_order_relaxed);
  }
  g_remaining.store(remaining, std::memory_order_relaxed);
}

void progress_update(uint64_t remaining, uint64_t total) {
  g_total.store(total > remaining ? total : remaining, std::memory_order_relaxed);
  g_remaining.store(remaining, std::memory_order_relaxed);
}

void progress_finish() { g_remaining.store(0, std::memory_order_relaxed); }

std::pair<uint64_t, uint64_t> progress_get() {
  uint64_t total = g_total.load(std::memory_order_relaxed);
  uint64_t remaining = g_remaining.load(std::memory_order_relaxed);
  uint64_t done = total >= remaining ? total - remaining : 0;
  return {done, total};
}

}  // namespace nlink
