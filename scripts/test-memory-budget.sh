#!/usr/bin/env bash
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/memory-budget-race.cpp" <<'EOF'
#include "memory_budget.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

int main() {
  constexpr int workers = 256;
  constexpr std::size_t mib = std::size_t(1) << 20;
  assert(membudget::configure("memory-budget-race", "TEST_MEMORY_BUDGET_MB"));
  const auto baseline = membudget::used();
  std::atomic<bool> go{false};
  std::atomic<int> ready{0};
  std::atomic<std::size_t> peak{0};
  std::atomic<int> successful{0}, attempted{0};
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (int i = 0; i < workers; ++i) {
    threads.emplace_back([&] {
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      try {
        membudget::charge(mib);
        successful.fetch_add(1, std::memory_order_relaxed);
        auto observed = membudget::used();
        auto old = peak.load(std::memory_order_relaxed);
        while (old < observed &&
               !peak.compare_exchange_weak(old, observed, std::memory_order_relaxed)) {}
        attempted.fetch_add(1);
        while (attempted.load() != workers) std::this_thread::yield();
        membudget::release(mib);
      } catch (const std::bad_alloc &) {
        attempted.fetch_add(1);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != workers) std::this_thread::yield();
  go.store(true, std::memory_order_release);
  for (auto &thread : threads) thread.join();
  // The first budget failure grants the documented 64 MiB reporting headroom.
  // Even with that headroom, reservations must not oversubscribe the limit.
  assert(peak.load() <= baseline + 192 * mib);
  assert(successful.load() <= 192);
}
EOF

c++ -std=c++17 -O2 -pthread -I"$root/cpp" "$tmp/memory-budget-race.cpp" \
  "$root/cpp/memory_budget.cpp" -o "$tmp/memory-budget-race"
TEST_MEMORY_BUDGET_MB=128 "$tmp/memory-budget-race" >/dev/null 2>&1
echo 'PASS memory-budget concurrent reservation test'
