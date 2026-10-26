// Replacement global operator new/delete that count bytes against the host memory budget.
// See memory_budget.hpp. Sizes come from the C allocator (malloc_usable_size, malloc_size,
// _msize), so a block allocated elsewhere and freed here only skews the count, never the heap.
#include "memory_budget.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace membudget {
namespace {

constexpr std::size_t kMiB = std::size_t(1) << 20;
constexpr std::size_t kDefaultMiB = 4096;
constexpr std::size_t kReserve = 64 * kMiB;  // headroom granted once so the error can be reported

std::atomic<std::int64_t> g_used{0};
std::atomic<std::size_t> g_limit{kDefaultMiB * kMiB};
std::atomic<bool> g_tripped{false};
const char *g_host = "host";
const char *g_env = "MAX_MEMORY_MB";

std::size_t clamp(std::int64_t v) { return v < 0 ? 0 : std::size_t(v); }

[[noreturn]] void trip(std::size_t request) {
  Exceeded e(request);
  if (!g_tripped.exchange(true)) {
    std::fprintf(stderr, "%s: %s\n", g_host, e.what());
    g_limit.fetch_add(kReserve);  // the error path may still allocate a little
  }
  throw e;
}

bool try_reserve(std::size_t request) {
  auto now = g_used.load(std::memory_order_relaxed);
  for (;;) {
    const auto lim = g_limit.load(std::memory_order_relaxed);
    if (request > lim || now > lim - request)
      return false;
    if (g_used.compare_exchange_weak(now, now + request, std::memory_order_relaxed,
                                     std::memory_order_relaxed))
      return true;
  }
}

void check(std::size_t request) {
  const std::size_t now = clamp(g_used.load(std::memory_order_relaxed));
  const std::size_t lim = g_limit.load(std::memory_order_relaxed);
  if (request > lim || now > lim - request) trip(request);
}

std::size_t block_size(void *p) {
#if defined(_WIN32)
  return _msize(p);
#elif defined(__APPLE__)
  return malloc_size(p);
#else
  return malloc_usable_size(p);
#endif
}

void *plain_alloc(std::size_t n) {
  n = n ? n : 1;
  if (!try_reserve(n)) trip(n);
  void *p = std::malloc(n);
  if (!p) {
    g_used.fetch_sub(std::int64_t(n), std::memory_order_relaxed);
    return nullptr;
  }
  const auto actual = block_size(p);
  if (actual > n) {
    if (!try_reserve(actual - n)) {
      g_used.fetch_sub(std::int64_t(n), std::memory_order_relaxed);
      std::free(p);
      trip(actual);
    }
  } else if (actual < n) {
    g_used.fetch_sub(std::int64_t(n - actual), std::memory_order_relaxed);
  }
  return p;
}

void plain_free(void *p) {
  if (!p) return;
  g_used.fetch_sub(std::int64_t(block_size(p)), std::memory_order_relaxed);
  std::free(p);
}

// Windows: _aligned_msize is missing from older msvcrt, so over-allocate with malloc and keep the
// raw pointer in front of the aligned block (libstdc++ is static there; nothing else frees it).
void *aligned_alloc_counted(std::size_t n, std::size_t align) {
  if (n == 0) n = 1;
  if (align < sizeof(void *)) align = sizeof(void *);
#if defined(_WIN32)
  if (n > SIZE_MAX - align - sizeof(void *)) return nullptr;
  const auto requested = n + align + sizeof(void *);
  if (!try_reserve(requested)) trip(requested);
  void *raw = std::malloc(requested);
  if (!raw) {
    g_used.fetch_sub(std::int64_t(requested), std::memory_order_relaxed);
    return nullptr;
  }
  std::uintptr_t start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void *);
  void *p = reinterpret_cast<void *>((start + align - 1) & ~std::uintptr_t(align - 1));
  static_cast<void **>(p)[-1] = raw;
  const auto actual = block_size(raw);
  if (actual > requested) {
    if (!try_reserve(actual - requested)) {
      g_used.fetch_sub(std::int64_t(requested), std::memory_order_relaxed);
      std::free(raw);
      trip(actual);
    }
  } else if (actual < requested) {
    g_used.fetch_sub(std::int64_t(requested - actual), std::memory_order_relaxed);
  }
  return p;
#else
  if (!try_reserve(n)) trip(n);
  void *p = nullptr;
  if (posix_memalign(&p, align, n) != 0) {
    g_used.fetch_sub(std::int64_t(n), std::memory_order_relaxed);
    return nullptr;
  }
  const auto actual = block_size(p);
  if (actual > n) {
    if (!try_reserve(actual - n)) {
      g_used.fetch_sub(std::int64_t(n), std::memory_order_relaxed);
      std::free(p);
      trip(actual);
    }
  } else if (actual < n) {
    g_used.fetch_sub(std::int64_t(n - actual), std::memory_order_relaxed);
  }
  return p;
#endif
}

void aligned_free_counted(void *p, std::size_t) {
  if (!p) return;
#if defined(_WIN32)
  void *raw = static_cast<void **>(p)[-1];
  g_used.fetch_sub(std::int64_t(block_size(raw)), std::memory_order_relaxed);
  std::free(raw);
#else
  plain_free(p);
#endif
}

}  // namespace

Exceeded::Exceeded(std::size_t request) {
  std::snprintf(msg_, sizeof msg_,
                "memory budget exceeded: %zu MiB in use, %zu MiB more requested, limit %zu MiB "
                "(set %s to raise it)",
                clamp(g_used.load()) / kMiB, (request + kMiB - 1) / kMiB,
                (g_limit.load() - (g_tripped.load() ? kReserve : 0)) / kMiB, g_env);
}

bool configure(const char *host, const char *env_var) {
  g_host = host;
  g_env = env_var;
  const char *v = std::getenv(env_var);
  if (!v || !*v) return true;
  char *end = nullptr;
  errno = 0;
  unsigned long long mib = std::strtoull(v, &end, 10);
  // at most 2^40 MiB keeps the byte count inside size_t on 64-bit hosts
  if (errno || *end || v[0] == '-' || v[0] == '+' || mib == 0 || mib > (1ull << 40) ||
      mib > SIZE_MAX / kMiB) {
    std::fprintf(stderr, "%s: %s must be a positive whole number of MiB, got '%s'\n", host, env_var, v);
    return false;
  }
  g_limit.store(std::size_t(mib) * kMiB);
  return true;
}

std::size_t used() { return clamp(g_used.load()); }
std::size_t limit() { return g_limit.load(); }
const char *env_var() { return g_env; }
void require(std::size_t bytes) { check(bytes); }
void charge(std::size_t bytes) {
  if (!try_reserve(bytes)) trip(bytes);
}
void release(std::size_t bytes) { g_used.fetch_sub(std::int64_t(bytes), std::memory_order_relaxed); }

}  // namespace membudget

// ---- replaceable global allocation functions ----

void *operator new(std::size_t n) {
  if (void *p = membudget::plain_alloc(n)) return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
  try { return membudget::plain_alloc(n); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t n, const std::nothrow_t &t) noexcept { return ::operator new(n, t); }
void operator delete(void *p) noexcept { membudget::plain_free(p); }
void operator delete[](void *p) noexcept { membudget::plain_free(p); }
void operator delete(void *p, std::size_t) noexcept { membudget::plain_free(p); }
void operator delete[](void *p, std::size_t) noexcept { membudget::plain_free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { membudget::plain_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { membudget::plain_free(p); }

void *operator new(std::size_t n, std::align_val_t a) {
  if (void *p = membudget::aligned_alloc_counted(n, std::size_t(a))) return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept {
  try { return membudget::aligned_alloc_counted(n, std::size_t(a)); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &t) noexcept {
  return ::operator new(n, a, t);
}
void operator delete(void *p, std::align_val_t a) noexcept { membudget::aligned_free_counted(p, std::size_t(a)); }
void operator delete[](void *p, std::align_val_t a) noexcept { membudget::aligned_free_counted(p, std::size_t(a)); }
void operator delete(void *p, std::size_t, std::align_val_t a) noexcept {
  membudget::aligned_free_counted(p, std::size_t(a));
}
void operator delete[](void *p, std::size_t, std::align_val_t a) noexcept {
  membudget::aligned_free_counted(p, std::size_t(a));
}
void operator delete(void *p, std::align_val_t a, const std::nothrow_t &) noexcept {
  membudget::aligned_free_counted(p, std::size_t(a));
}
void operator delete[](void *p, std::align_val_t a, const std::nothrow_t &) noexcept {
  membudget::aligned_free_counted(p, std::size_t(a));
}
