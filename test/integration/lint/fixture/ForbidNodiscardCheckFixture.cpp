#include "TestHelpers.h"

// POSITIVE
[[nodiscard]] int getForbiddenVal()
{
  return 42;
}

// POSITIVE
struct [[nodiscard]] ForbiddenStruct
{};

// POSITIVE
class [[nodiscard]] ForbiddenClass
{};

// NEGATIVE
int getConformingVal()
{
  return 42;
}
