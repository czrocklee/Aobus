#include "TestHelpers.h"

#include <mutex>
#include <thread>

void testThreadingPolicy()
{
  // POSITIVE
  std::thread t([] {});
  t.join();

  // NEGATIVE
  std::jthread jt([] {});

  // POSITIVE
  [[maybe_unused]] int volatile volatileVar = 0;

  std::mutex m;

  // POSITIVE
  [[maybe_unused]] std::unique_lock<std::mutex> redundantLock(m);

  // NEGATIVE
  [[maybe_unused]] std::unique_lock<std::mutex> deferLock(m, std::defer_lock);

  // NEGATIVE
  std::unique_lock<std::mutex> manualLock(m);
  manualLock.unlock();
}
