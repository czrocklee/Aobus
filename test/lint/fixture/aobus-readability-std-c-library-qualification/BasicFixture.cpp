// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cmath>
#include <cstdlib>
#include <cstring>

// Custom system-like C function
extern "C" void system_like_c_func()
{}

#define AOBUS_STRLEN strlen

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

  // POSITIVE
  [[maybe_unused]] size_t const len3 = ::strlen("Hello");

  // POSITIVE
  [[maybe_unused]] size_t const len4 = (strlen)("Hello");

  // POSITIVE
  [[maybe_unused]] size_t const len5 = (::strlen)("Hello");

  // NEGATIVE - callee spelled by a macro, so the checker abstains
  [[maybe_unused]] size_t const len6 = (AOBUS_STRLEN)("Hello");

  // NEGATIVE
  std::free(p);
}
