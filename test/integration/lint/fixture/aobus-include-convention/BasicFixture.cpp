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

int main()
{
  (void)"ao/Error.h";
  return 0;
}
