#include <cstdint>
#include <iostream>

static void testUseIfInit(std::int32_t cond)
{
  // POSITIVE
  std::int32_t const localX = cond * 2;

  if (localX > 10)
  {
    std::cout << localX;
  }

  // POSITIVE
  std::int32_t const localSwitch = cond + 1;

  switch (localSwitch)
  {
    case 1: break;
    default: break;
  }

  // NEGATIVE
  std::int32_t const usedAfter = cond * 3;

  if (usedAfter > 0)
  {
    std::cout << usedAfter;
  }

  std::cout << usedAfter;

  // NEGATIVE
  constexpr std::int32_t kConstVar = 42;

  if (kConstVar > 0)
  {
    std::cout << kConstVar;
  }

  // NEGATIVE
  if (std::int32_t const localY = cond * 2; localY > 10)
  {
    std::cout << localY;
  }
}
