// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// POSITIVE: FIX-TO: #include <ao/Error.h>
#include "ao/Error.h"

// POSITIVE: FIX-TO: #include <vector>
#include "vector"

// NEGATIVE
#include <ao/Error.h>

// NEGATIVE
#include <vector>

extern "C"
{
// NEGATIVE
#include <stdint.h>
}

namespace include_convention_fixture
{
  class ForwardDeclared;
}

// POSITIVE
#include <array>

int main()
{
  (void)"ao/Error.h";
  return 0;
}
