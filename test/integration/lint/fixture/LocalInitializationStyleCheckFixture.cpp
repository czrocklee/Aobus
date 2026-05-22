#include "TestHelpers.h"

#include <string>
#include <vector>

void testLocalInitialization()
{
  using namespace std::string_literals;
  using namespace std::string_view_literals;

  // POSITIVE
  LocalFoo explicitLocal{10};

  // POSITIVE
  std::string explicitStr("hello");

  // POSITIVE
  std::string_view explicitSv("world");

  // POSITIVE
  std::vector<int> explicitVec(10, 2);

  // POSITIVE
  int bracedPrimitive{5};

  // NEGATIVE
  [[maybe_unused]] auto modernVal = int{5};
  [[maybe_unused]] auto optStr = "hello"s;
  [[maybe_unused]] auto optSv = "world"sv;
  [[maybe_unused]] auto optVec = std::vector<int>(10, 2);
  [[maybe_unused]] int optPrimitive = 5;

  // NEGATIVE
  [[maybe_unused]] int* optPointer = nullptr;
}
