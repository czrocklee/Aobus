// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Run.h"

#include <iostream>

using namespace ao;

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char const* argv[])
{
  return cli::run(argc, argv, std::cout, std::cerr);
}
