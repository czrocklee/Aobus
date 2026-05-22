#include "TestHelpers.h"

void testUnusedSuppression(int param)
{
  // POSITIVE
  (void)param;

  // POSITIVE
  static_cast<void>(param);

  // NEGATIVE
  [[maybe_unused]] int param2 = 10;
}
