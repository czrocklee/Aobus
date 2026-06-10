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
  // POSITIVE: FIX-TO: [[maybe_unused]] size_t const len = std::strlen("Hello");
  [[maybe_unused]] size_t const len = strlen("Hello");

  // POSITIVE: FIX-TO: [[maybe_unused]] double const d = std::sin(3.14);
  [[maybe_unused]] double const d = sin(3.14);

  // NEGATIVE
  [[maybe_unused]] std::size_t const len2 = std::strlen("Hello");

  // NEGATIVE
  [[maybe_unused]] double const d2 = std::sin(3.14);

  // POSITIVE: FIX-TO: [[maybe_unused]] void* p = std::malloc(10);
  [[maybe_unused]] void* p = malloc(10);

  // POSITIVE: FIX-TO: [[maybe_unused]] size_t const len3 = std::strlen("Hello");
  [[maybe_unused]] size_t const len3 = ::strlen("Hello");

  // POSITIVE: FIX-TO: [[maybe_unused]] size_t const len4 = (std::strlen)("Hello");
  [[maybe_unused]] size_t const len4 = (strlen)("Hello");

  // POSITIVE: FIX-TO: [[maybe_unused]] size_t const len5 = (std::strlen)("Hello");
  [[maybe_unused]] size_t const len5 = (::strlen)("Hello");

  // NEGATIVE - callee spelled by a macro; a FixIt would edit the macro definition
  [[maybe_unused]] size_t const len6 = (AOBUS_STRLEN)("Hello");

  // NEGATIVE
  std::free(p);
}
