#include <string>

using namespace std::string_literals;

void positiveFindStringEqZero()
{
  auto str = "hello world"s;
  // POSITIVE
  if (str.find("hello") == 0)
  {
  }
}

void positiveZeroEqFindString()
{
  auto str = "hello world"s;
  // POSITIVE
  if (0 == str.find("hello"))
  {
  }
}

void positiveFindCharEqZero()
{
  auto str = "hello world"s;
  // POSITIVE
  if (str.find('h') == 0)
  {
  }
}

void positiveFindNeqZero()
{
  auto str = "hello world"s;
  // POSITIVE
  if (str.find("hello") != 0)
  {
  }
}

void negativeFindEqOne()
{
  auto str = "hello world"s;
  // NEGATIVE
  if (str.find("hello") == 1)
  {
  }
}

void negativeFindNoCompare()
{
  auto str = "hello world"s;
  auto pos = str.find("hello");
  // NEGATIVE: no comparison with 0
  (void)pos;
}
