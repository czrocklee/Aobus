
#include <mutex>
#include <thread>

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
