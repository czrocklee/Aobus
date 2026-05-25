#include <cmath>
#include <cstdlib>
#include <cstring>

// Custom system-like C function
extern "C" void system_like_c_func()
{}

void testStdCQualification()
{
  // POSITIVE
  [[maybe_unused]] size_t const len = strlen("Hello");

  // POSITIVE
  [[maybe_unused]] double const d = sin(3.14);

  // NEGATIVE
  [[maybe_unused]] std::size_t const len2 = std::strlen("Hello");

  // NEGATIVE
  [[maybe_unused]] double const d2 = std::sin(3.14);

  // POSITIVE
  [[maybe_unused]] void* p = malloc(10);

  // NEGATIVE
  std::free(p);
}
