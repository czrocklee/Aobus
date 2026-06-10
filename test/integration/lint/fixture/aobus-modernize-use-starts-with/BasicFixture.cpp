// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <string>
#include <string_view>

using namespace std::string_literals;

// A find() member that reports a match count, not a position; the type has no
// starts_with() member.
struct Config
{
  int find(std::string_view key) const;
};

#define AOBUS_AT_START(s, sub) (s.find(sub) == 0)

void positiveFindStringEqZero()
{
  auto str = "hello world"s;
  // POSITIVE: FIX-TO: if (str.starts_with("hello"))
  if (str.find("hello") == 0)
  {
  }
}

void positiveZeroEqFindString()
{
  auto str = "hello world"s;
  // POSITIVE: FIX-TO: if (str.starts_with("hello"))
  if (0 == str.find("hello"))
  {
  }
}

void positiveFindCharEqZero()
{
  auto str = "hello world"s;
  // POSITIVE: FIX-TO: if (str.starts_with('h'))
  if (str.find('h') == 0)
  {
  }
}

void positiveFindNeqZero()
{
  auto str = "hello world"s;
  // POSITIVE: FIX-TO: if (!str.starts_with("hello"))
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
  // NEGATIVE - no comparison with 0
  (void)pos;
}

void negativeCustomFindType(Config const& cfg)
{
  // NEGATIVE - Config::find is not a position lookup and Config has no starts_with()
  if (cfg.find("hello") == 0)
  {
  }
}

void negativeMacroSpelled()
{
  auto str = "hello world"s;
  // NEGATIVE - the find-and-compare is spelled inside a macro expansion
  if (AOBUS_AT_START(str, "hello"))
  {
  }
}
