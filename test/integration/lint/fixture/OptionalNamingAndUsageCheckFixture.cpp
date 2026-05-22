#include "TestHelpers.h"

#include <optional>

void testOptionalUsage(std::optional<int> optVal)
{
  // POSITIVE
  [[maybe_unused]] std::optional<int> invalidOptName = 42;

  // NEGATIVE
  [[maybe_unused]] auto optValidName = std::optional<int>{42};

  // POSITIVE
  if (optVal.has_value())
  {
    [[maybe_unused]] int val = *optVal;
  }

  // NEGATIVE
  if (optVal)
  {
    [[maybe_unused]] int safeVal = *optVal;
  }
}
