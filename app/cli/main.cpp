// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Run.h"

#ifdef _MSC_VER
// Linking mimalloc alone does not replace MSVC's global C++ allocation
// operators. Define the forwarding operators in an executable TU so /OPT:REF
// cannot discard the mimalloc reference.
#include <mimalloc-new-delete.h> // NOLINT(misc-include-cleaner) -- installs global allocation operators
#endif

#include <iostream>

using namespace ao;

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char const* argv[])
{
  return cli::run(argc, argv, std::cout, std::cerr);
}
