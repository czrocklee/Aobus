// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <string>
#include <string_view>

using namespace std::string_literals;

// Truth of the starts_with() member. Stored find results are not comparisons,
// so these asserts are not diagnostics.
namespace
{
  constexpr auto kText = std::string_view{"hello world"};
  constexpr auto kHello = kText.find("hello");
  constexpr auto kMissing = kText.find("xyz");
  constexpr auto kFromZero = kText.find("hello", 0);

  static_assert(kText.starts_with("hello"));
  static_assert(!kText.starts_with("xyz"));
  static_assert((kHello == 0) == kText.starts_with("hello"));
  static_assert((kMissing != 0) == !kText.starts_with("xyz"));
  static_assert((0 == kHello) == kText.starts_with("hello"));
  static_assert((0 != kMissing) == !kText.starts_with("xyz"));
  static_assert((kFromZero == 0) == kText.starts_with("hello"));

  // The three-argument find() truncates its needle to the given count, so a
  // position of 0 is only a prefix of the truncated needle, not of the whole
  // needle. starts_with() would change the meaning of the comparison.
  constexpr auto kSingle = std::string_view{"a"};
  static_assert(kSingle.find("ab", 0, 1) == 0);
  static_assert(!kSingle.starts_with("ab"));
} // namespace

// A find() member that reports a match count, not a position; the type has no
// starts_with() member.
struct Config
{
  int find(std::string_view key) const;
};

#define AOBUS_AT_START(s, sub) (s.find(sub) == 0)

// Argument-list macro: the needle and position cover part of the expansion
// while the comparison stays file-spelled.
#define AOBUS_FIND_ARGS "pre", 0

void positiveFindStringEqZero()
{
  auto str = "hello world"s;
  // POSITIVE | use starts_with instead of find
  if (str.find("hello") == 0)
  {
  }
}

void positiveZeroEqFindString()
{
  auto str = "hello world"s;
  // POSITIVE | use starts_with instead of find
  if (0 == str.find("hello"))
  {
  }
}

void positiveFindCharEqZero()
{
  auto str = "hello world"s;
  // POSITIVE | use starts_with instead of find
  if (str.find('h') == 0)
  {
  }
}

void positiveFindNeqZero()
{
  auto str = "hello world"s;
  // The stock title alone does not express negation. The Aobus fixture
  // separately asserts explicit !starts_with() diagnostic text.
  // POSITIVE | use starts_with instead of find
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
  // NEGATIVE - no comparison with 0
  auto pos = str.find("hello");
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

void positiveZeroNeqFindString()
{
  auto const str = "hello world"s;
  // POSITIVE | use starts_with instead of find
  if (0 != str.find("hello"))
  {
  }
}

void positiveStringViewEqZero()
{
  auto const text = std::string_view{"hello world"};
  // POSITIVE | use starts_with instead of find
  if (text.find("hello") == 0)
  {
  }
}

void positiveFindPosZero()
{
  auto const str = "hello world"s;
  // POSITIVE | use starts_with instead of find
  if (str.find("hello", 0) == 0)
  {
  }
}

void positiveFindPartialMacroArgs()
{
  auto const str = "prefix"s;
  // The stock fix-it spans the macro expansion; this control asserts the
  // diagnostic only, and the portal does not apply stock fixes.
  // POSITIVE | use starts_with instead of find
  if (str.find(AOBUS_FIND_ARGS) != 0)
  {
  }
}

void negativeFindTruncatedNeedle()
{
  auto const text = std::string_view{"a"};
  // The third argument truncates the needle; position 0 does not establish
  // that the whole needle is a prefix.
  // NEGATIVE
  if (text.find("ab", 0, 1) == 0)
  {
  }
}

void positiveRfindPosZero()
{
  auto const str = "hello world"s;
  // POSITIVE | use starts_with instead of rfind
  if (str.rfind("hello", 0) == 0)
  {
  }
}

void positiveRfindEndsWith()
{
  auto const str = "hello world"s;
  // POSITIVE | use ends_with instead of rfind
  if (str.rfind("world") == str.size() - 5)
  {
  }
}

void positiveCompareEqZero()
{
  auto const str = "hello world"s;
  // POSITIVE | use starts_with instead of compare
  if (str.compare(0, 5, "hello") == 0)
  {
  }
}
