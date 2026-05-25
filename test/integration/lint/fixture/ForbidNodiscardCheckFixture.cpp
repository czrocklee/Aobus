#include <cstdint>

// POSITIVE
[[nodiscard]] std::int32_t getForbiddenVal()
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
std::int32_t getConformingVal()
{
  return 42;
}
