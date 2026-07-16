// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <mutex>
#include <thread>

// NEGATIVE: a parameter carries an established lock lifetime across a call boundary.
void consumeLock(std::unique_lock<std::mutex> /*lock*/)
{
}

void testThreadingPolicy()
{
  // POSITIVE
  auto t = std::thread{[] {}};
  t.join();

  // NEGATIVE
  auto jt = std::jthread{[] {}};

  // POSITIVE
  [[maybe_unused]] int const volatile volatileVar = 0;

  auto m = std::mutex{};

  // POSITIVE
  [[maybe_unused]] auto redundantLock = std::unique_lock<std::mutex>{m};

  // NEGATIVE
  [[maybe_unused]] auto deferLock = std::unique_lock<std::mutex>{m, std::defer_lock};

  // NEGATIVE
  auto manualLock = std::unique_lock<std::mutex>{m};
  manualLock.unlock();
}
