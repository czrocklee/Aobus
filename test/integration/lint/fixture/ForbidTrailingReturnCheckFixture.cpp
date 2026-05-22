#include "TestHelpers.h"

// POSITIVE
auto testTrailing() -> int
{
  return 10;
}

// POSITIVE
auto testNecessaryTrailing() -> decltype(auto)
{
  static int x = 5;
  return (x);
}

// NEGATIVE
auto lambdaWithTrailing = [](int x) -> double { return x * 1.0; };

// NEGATIVE
template<typename T>
struct DeductionDemo
{
  DeductionDemo(T) {}
};
DeductionDemo(int) -> DeductionDemo<double>;
