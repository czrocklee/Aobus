#include <cstdint>

void testUnusedSuppression(std::int32_t param)
{
  // POSITIVE
  (void)param;

  // POSITIVE
  static_cast<void>(param);

  // NEGATIVE
  [[maybe_unused]] std::int32_t const param2 = 10;
}
