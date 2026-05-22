#include "TestHelpers.h"

#include <iostream>

void testLambdaParams()
{
  // POSITIVE
  [[maybe_unused]] auto invalidLambda = []() { std::cout << "Hi"; };

  // NEGATIVE
  [[maybe_unused]] auto validLambda = [] { std::cout << "Hi"; };

  // NEGATIVE
  [[maybe_unused]] auto paramsLambda = [](int x) { std::cout << x; };
}
