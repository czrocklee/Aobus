// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// FIX-TO: #include <cstdint>\n\n#include "TestHelpers.h"
#include "TestHelpers.h"

class MyWidget : public gtkmm_mock::Widget
{
public:
  // Should ignore because it overrides a virtual function
  void on_draw(int x) override {}
};

extern "C"
{
  // Should ignore inside extern "C"
  int global_c_function(int a)
  {
    return a;
  }
}

// POSITIVE: FIX-TO: std::int32_t global_var = 0;
int global_var = 0;

// POSITIVE: FIX-TO: std::int16_t small_var = 0;
short small_var = 0;

// POSITIVE: FIX-TO: std::uint32_t u_var = 0;
unsigned int u_var = 0;

struct AlsaMock
{
  // Should ignore because it's used in callAlsa below
  long volumeMin = 0;

  // POSITIVE
  long unrelatedLong = 0;

  void callAlsa() { some_c_api(&volumeMin, 1); }
};

void testLogic()
{
  // POSITIVE: FIX-TO: std::int32_t local = 0;
  int local = 0;

  // POSITIVE
  long big = 0; // WARN only, no fix

  // Ignored because it's passed to C API via pointer
  long val_for_c = 0;
  some_c_api(&val_for_c, 5);
}

// main is ignored
int main(int argc, char** argv)
{
  return 0;
}
