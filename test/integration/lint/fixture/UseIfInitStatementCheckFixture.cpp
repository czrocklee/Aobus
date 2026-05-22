#include "TestHelpers.h"

#include <iostream>

void testUseIfInit(int cond)
{
  // POSITIVE
  int localX = cond * 2;

  if (localX > 10)
  {
    std::cout << localX;
  }

  // POSITIVE
  int localSwitch = cond + 1;

  switch (localSwitch)
  {
    case 1: break;
    default: break;
  }

  // NEGATIVE
  int usedAfter = cond * 3;

  if (usedAfter > 0)
  {
    std::cout << usedAfter;
  }

  std::cout << usedAfter;

  // NEGATIVE
  constexpr int constVar = 42;

  if (constVar > 0)
  {
    std::cout << constVar;
  }

  // NEGATIVE
  if (int localY = cond * 2; localY > 10)
  {
    std::cout << localY;
  }
}
