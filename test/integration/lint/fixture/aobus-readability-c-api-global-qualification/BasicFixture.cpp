// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <unistd.h>

// NOLINTBEGIN(readability-identifier-naming)

extern "C" void my_local_c_function()
{}

#define AOBUS_GETPID getpid

namespace
{
  template<auto Fn>
  int callThroughTemplateParam()
  {
    // NEGATIVE - routed through a non-type template parameter, not a C function
    return Fn();
  }

  void testCApiQualification()
  {
    // POSITIVE: FIX-TO: ::getpid();
    getpid();

    // POSITIVE: FIX-TO: (::getpid)();
    (getpid)();

    // NEGATIVE - callee spelled by a macro; a FixIt would edit the macro definition
    (AOBUS_GETPID)();

    // NEGATIVE
    ::getpid();

    // NEGATIVE - Declared in the project, not system headers
    my_local_c_function();

    // Force instantiation so the check visits the template body above
    (void)callThroughTemplateParam<::getpid>();
  }
} // namespace

// NOLINTEND(readability-identifier-naming)
