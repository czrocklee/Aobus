// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstddef>
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
  constexpr auto kFullCount = kText.find("hello", 0, 5);
  constexpr auto kStartPosition = std::size_t{0};

  static_assert(kText.starts_with("hello"));
  static_assert(!kText.starts_with("xyz"));
  static_assert((kHello == 0) == kText.starts_with("hello"));
  static_assert((kMissing != 0) == !kText.starts_with("xyz"));
  static_assert((0 == kHello) == kText.starts_with("hello"));
  static_assert((0 != kMissing) == !kText.starts_with("xyz"));
  static_assert((kFromZero == 0) == kText.starts_with("hello"));

  // A count that covers the exact literal keeps the position-0 meaning.
  static_assert((kFullCount == 0) == kText.starts_with("hello"));

  // A truncated count reports a prefix of the needle only, so position 0
  // does not state that the whole needle is a prefix.
  constexpr auto kSingle = std::string_view{"a"};
  static_assert(kSingle.find("ab", 0, 1) == 0); // NEGATIVE - the count truncates the needle
  static_assert(!kSingle.starts_with("ab"));

  // An embedded NUL ends the measured needle early, so the full-count
  // comparison and starts_with() disagree about the same position.
  constexpr auto kNulPrefix = std::string_view{"hel"};
  static_assert(kNulPrefix.find("hel\0lo", 0, 6) != 0); // NEGATIVE - the needle holds a NUL code unit
  static_assert(kNulPrefix.starts_with("hel\0lo"));
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
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(str.find("hello") == 0);
}

void positiveZeroEqFindString()
{
  auto str = "hello world"s;
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(0 == str.find("hello"));
}

void positiveFindCharEqZero()
{
  auto str = "hello world"s;
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(str.find('h') == 0);
}

void positiveFindNeqZero()
{
  auto str = "hello world"s;
  // POSITIVE | use !starts_with() member instead of find != 0
  (void)(str.find("hello") != 0);
}

void negativeFindEqOne()
{
  auto str = "hello world"s;
  // NEGATIVE
  (void)(str.find("hello") == 1);
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
  (void)(cfg.find("hello") == 0);
}

void negativeMacroSpelled()
{
  auto str = "hello world"s;
  // NEGATIVE - the find-and-compare is spelled inside a macro expansion
  (void)(AOBUS_AT_START(str, "hello"));
}

void positiveZeroNeqFindString()
{
  auto const str = "hello world"s;
  // POSITIVE | use !starts_with() member instead of find != 0
  (void)(0 != str.find("hello"));
}

void positiveStringViewEqZero()
{
  auto const text = std::string_view{"hello world"};
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(text.find("hello") == 0);
}

void positiveFindPosZero()
{
  auto const str = "hello world"s;
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(str.find("hello", 0) == 0);
}

void positiveFindPartialMacroArgs()
{
  auto const str = "prefix"s;
  // POSITIVE | use !starts_with() member instead of find != 0
  (void)(str.find(AOBUS_FIND_ARGS) != 0);
}

void positiveFindConstantPosition()
{
  auto const str = "hello world"s;
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(str.find("hello", kStartPosition) == 0);
}

void positiveFindSizedPosition()
{
  auto const str = "hello world"s;
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(str.find("hello", std::size_t{0}) == 0);
}

void positiveFindFoldedPosition()
{
  auto const str = "hello world"s;
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(str.find("hello", 2 - 2) == 0);
}

void positiveFindFullNeedleCountEqZero()
{
  auto const str = "hello world"s;
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(str.find("hello", 0, 5) == 0);
}

void positiveFindFullNeedleCountNeqZero()
{
  auto const str = "hello world"s;
  // POSITIVE | use !starts_with() member instead of find != 0
  (void)(str.find("hello", 0, 5) != 0);
}

void negativeFindUnprovedCount(std::size_t needleLength)
{
  auto const str = "hello world"s;
  // NEGATIVE - a runtime count is not proven to cover the whole needle
  (void)(str.find("hello", 0, needleLength) == 0);
}

void negativeFindTruncatedCount()
{
  auto const str = "hello world"s;
  // The count truncates the needle to "hel", not the whole needle.
  // NEGATIVE
  (void)(str.find("hello", 0, 3) == 0);
}

void negativeFindUnknownNeedle(char const* needle)
{
  auto const text = std::string_view{"hello world"};
  // NEGATIVE - a count alone cannot prove an unknown needle's full length
  (void)(text.find(needle, 0, 3) == 0);
}

void positiveFindWideFullCount()
{
  auto const text = std::u16string_view{u"hello world"};
  // POSITIVE | use starts_with() member instead of find == 0
  (void)(text.find(u"hello", 0, 5) == 0);
}

void negativeFindWideEmbeddedNul()
{
  auto const text = std::u16string_view{u"hello world"};
  // NEGATIVE - code-unit width does not change the NUL boundary
  (void)(text.find(u"hel\0lo", 0, 6) == 0);
}

void negativeFindNeedleWithEmbeddedNul()
{
  auto const str = "hello world"s;
  // starts_with() measures the needle only up to the embedded NUL.
  // NEGATIVE
  (void)(str.find("hel\0lo", 0, 6) == 0);
}
