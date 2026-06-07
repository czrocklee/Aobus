// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <unistd.h>

// NOLINTBEGIN(readability-identifier-naming)

extern "C" void my_local_c_function()
{}

namespace
{
  void testCApiQualification()
  {
    // POSITIVE
    getpid();

    // NEGATIVE
    ::getpid();

    // NEGATIVE - Declared in the project, not system headers
    my_local_c_function();
  }
} // namespace

// NOLINTEND(readability-identifier-naming)
