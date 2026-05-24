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

// POSITIVE
int global_var = 0;

// POSITIVE
short small_var = 0;

// POSITIVE
unsigned int u_var = 0;

struct AlsaMock
{
  // Should ignore because it's used in callAlsa below
  long volumeMin = 0;

  // POSITIVE
  long unrelatedLong = 0;

  void callAlsa()
  {
    some_c_api(&volumeMin, 1);
  }
};

void testLogic()
{
  // POSITIVE
  int local = 0;

  // POSITIVE
  long big = 0; // WARN only, no fix

  // Ignored because it's passed to C API via pointer
  long val_for_c = 0;
  some_c_api(&val_for_c, 5);

  // POSITIVE
  auto cast_val = static_cast<long>(10);
}

// main is ignored
int main(int argc, char** argv)
{
  return 0;
}
