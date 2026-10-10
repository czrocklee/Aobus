// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/RecordingDate.h>

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    RecordingDate requireParsed(std::string_view text)
    {
      auto res = parseRecordingDate(text);
      REQUIRE(res);
      return *res;
    }

    void requireRejected(std::string_view text)
    {
      auto res = parseRecordingDate(text);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
    }

    // Collapses the strong-ordering result onto -1/0/1 so assertions read
    // like the report's =, <, and > operator outcomes.
    std::int32_t compareSign(RecordingDate stored, RecordingDate literal)
    {
      auto const optOrder = compareRecordingDateAtLiteralPrecision(stored, literal);
      REQUIRE(optOrder);

      if (*optOrder < 0)
      {
        return -1;
      }

      return *optOrder > 0 ? 1 : 0;
    }
  } // namespace

  TEST_CASE("parseRecordingDate - accepts canonical literals at each precision", "[library][unit][recording-date]")
  {
    auto const yearOnly = requireParsed("1981");
    CHECK(yearOnly.year == 1981);
    CHECK(yearOnly.month == 0);
    CHECK(yearOnly.day == 0);
    CHECK(yearOnly.isPresent());
    CHECK(yearOnly.isValid());

    auto const monthPrecision = requireParsed("1981-05");
    CHECK(monthPrecision.year == 1981);
    CHECK(monthPrecision.month == 5);
    CHECK(monthPrecision.day == 0);

    auto const fullDate = requireParsed("1981-05-12");
    CHECK(fullDate.year == 1981);
    CHECK(fullDate.month == 5);
    CHECK(fullDate.day == 12);

    SECTION("admitted year range")
    {
      auto const earliest = requireParsed("0001");
      CHECK(earliest.year == 1);
      CHECK(earliest.isValid());

      auto const latest = requireParsed("9999");
      CHECK(latest.year == 9999);
      CHECK(latest.isValid());
    }
  }

  TEST_CASE("parseRecordingDate - rejects malformed date text", "[library][unit][recording-date]")
  {
    SECTION("empty and whitespace text")
    {
      requireRejected("");
      requireRejected(" ");
      requireRejected(" 1981");
      requireRejected("1981 ");
      requireRejected("1981\t");
      requireRejected("1981\n");
      requireRejected("1981-05-12 ");
      requireRejected(" 1981-05-12");
    }

    SECTION("noncanonical widths and padding")
    {
      requireRejected("981");
      requireRejected("19811");
      requireRejected("1981-5");
      requireRejected("1981-05-1");
      requireRejected("1981-5-12");
      requireRejected("1981-050-12");
      requireRejected("1981-05-123");
    }

    SECTION("wrong or missing separators")
    {
      requireRejected("1981/05/12");
      requireRejected("1981.05.12");
      requireRejected("1981_05_12");
      requireRejected("1981--05");
      requireRejected("1981- 5");
    }

    SECTION("time and zone suffixes")
    {
      requireRejected("1981-05-12T00:00");
      requireRejected("1981-05-12T00:00:00");
      requireRejected("1981-05-12 00:00");
      requireRejected("1981-05-12Z");
    }

    SECTION("non-ASCII digits")
    {
      requireRejected("١٩٨١");
      requireRejected("１９８１");
      requireRejected("1981-０٥");
    }

    SECTION("signs and stray characters")
    {
      requireRejected("+981");
      requireRejected("-981");
      requireRejected("19-81");
    }

    SECTION("year zero")
    {
      requireRejected("0000");
      requireRejected("0000-05");
      requireRejected("0000-05-12");
    }
  }

  TEST_CASE("parseRecordingDate - validates real month lengths and leap years", "[library][unit][recording-date]")
  {
    SECTION("leap years admit February 29")
    {
      CHECK(requireParsed("0004-02-29").day == 29);
      CHECK(requireParsed("0400-02-29").day == 29);
      CHECK(requireParsed("1904-02-29").day == 29);
      CHECK(requireParsed("1996-02-29").day == 29);
      CHECK(requireParsed("2000-02-29").day == 29);
      CHECK(requireParsed("2400-02-29").day == 29);
    }

    SECTION("non-leap years reject February 29")
    {
      requireRejected("0001-02-29");
      requireRejected("0100-02-29");
      requireRejected("1900-02-29");
      requireRejected("1981-02-29");
      requireRejected("1999-02-29");
      requireRejected("2100-02-29");
    }

    SECTION("February 28 stays valid in every year")
    {
      CHECK(requireParsed("1900-02-28").day == 28);
      CHECK(requireParsed("2000-02-28").day == 28);
    }

    SECTION("thirty-day months reject day 31")
    {
      CHECK(requireParsed("1981-04-30").day == 30);
      requireRejected("1981-04-31");
      requireRejected("1981-06-31");
      requireRejected("1981-09-31");
      requireRejected("1981-11-31");
    }

    SECTION("thirty-one-day months admit day 31")
    {
      CHECK(requireParsed("1981-01-31").day == 31);
      CHECK(requireParsed("1981-05-31").day == 31);
      CHECK(requireParsed("1981-12-31").day == 31);
    }

    SECTION("month and day ranges")
    {
      requireRejected("1981-00");
      requireRejected("1981-13");
      requireRejected("1981-00-05");
      requireRejected("1981-05-00");
      requireRejected("1981-05-32");
      requireRejected("1981-02-30");
    }
  }

  TEST_CASE("RecordingDate - zero is the absence sentinel", "[library][unit][recording-date]")
  {
    auto const absent = RecordingDate{};
    CHECK_FALSE(absent.isPresent());
    CHECK(absent.isValid());

    CHECK(RecordingDate{.year = 0, .month = 0, .day = 0}.isValid());

    SECTION("components without their predecessor are invalid")
    {
      CHECK_FALSE(RecordingDate{.year = 0, .month = 5, .day = 0}.isValid());
      CHECK_FALSE(RecordingDate{.year = 0, .month = 0, .day = 12}.isValid());
      CHECK_FALSE(RecordingDate{.year = 0, .month = 5, .day = 12}.isValid());
      CHECK_FALSE(RecordingDate{.year = 1981, .month = 0, .day = 12}.isValid());
    }

    SECTION("out-of-range components are invalid")
    {
      CHECK_FALSE(RecordingDate{.year = 10000, .month = 1, .day = 1}.isValid());
      CHECK_FALSE(RecordingDate{.year = 65535, .month = 1, .day = 1}.isValid());
      CHECK_FALSE(RecordingDate{.year = 1981, .month = 13, .day = 0}.isValid());
      CHECK_FALSE(RecordingDate{.year = 1981, .month = 13, .day = 1}.isValid());
      CHECK_FALSE(RecordingDate{.year = 1981, .month = 2, .day = 29}.isValid());
      CHECK_FALSE(RecordingDate{.year = 1981, .month = 4, .day = 31}.isValid());
      CHECK_FALSE(RecordingDate{.year = 1981, .month = 5, .day = 32}.isValid());
    }

    SECTION("valid partial and complete dates")
    {
      CHECK(RecordingDate{.year = 1, .month = 1, .day = 1}.isValid());
      CHECK(RecordingDate{.year = 1981, .month = 5, .day = 0}.isValid());
      CHECK(RecordingDate{.year = 2000, .month = 2, .day = 29}.isValid());
      CHECK(RecordingDate{.year = 9999, .month = 12, .day = 31}.isValid());
    }
  }

  TEST_CASE("RecordingDate - exact equality includes the stored precision", "[library][unit][recording-date]")
  {
    // 1981, 1981-05, and 1981-05-12 are three different stored values.
    CHECK(RecordingDate{.year = 1981, .month = 0, .day = 0} == RecordingDate{.year = 1981, .month = 0, .day = 0});
    CHECK(RecordingDate{.year = 1981, .month = 0, .day = 0} != RecordingDate{.year = 1981, .month = 5, .day = 0});
    CHECK(RecordingDate{.year = 1981, .month = 0, .day = 0} != RecordingDate{.year = 1981, .month = 5, .day = 12});
    CHECK(RecordingDate{.year = 1981, .month = 5, .day = 0} != RecordingDate{.year = 1981, .month = 5, .day = 12});

    CHECK(RecordingDate{} == RecordingDate{});
    CHECK(RecordingDate{} != RecordingDate{.year = 1981, .month = 0, .day = 0});
    CHECK(requireParsed("1981-05-12") == RecordingDate{.year = 1981, .month = 5, .day = 12});
    CHECK(requireParsed("1981-05") != requireParsed("1981-05-12"));
  }

  TEST_CASE("formatRecordingDate - preserves the stored precision", "[library][unit][recording-date]")
  {
    CHECK(formatRecordingDate(RecordingDate{}).empty());
    CHECK(formatRecordingDate(RecordingDate{.year = 1981, .month = 0, .day = 0}) == "1981");
    CHECK(formatRecordingDate(RecordingDate{.year = 1981, .month = 5, .day = 0}) == "1981-05");
    CHECK(formatRecordingDate(RecordingDate{.year = 1981, .month = 5, .day = 12}) == "1981-05-12");
    CHECK(formatRecordingDate(RecordingDate{.year = 1, .month = 1, .day = 1}) == "0001-01-01");
    CHECK(formatRecordingDate(RecordingDate{.year = 400, .month = 2, .day = 29}) == "0400-02-29");
    CHECK(formatRecordingDate(RecordingDate{.year = 9999, .month = 12, .day = 31}) == "9999-12-31");

    auto const canonicalLiterals = std::vector<std::string_view>{
      "0001",
      "1981",
      "1981-05",
      "1981-05-12",
      "0004-02-29",
      "0400-02-29",
      "2000-02-29",
      "9999-12-31",
    };

    for (auto const literal : canonicalLiterals)
    {
      CHECK(formatRecordingDate(requireParsed(literal)) == literal);
    }
  }

  TEST_CASE("compareRecordingDateAtLiteralPrecision - compares a stored date at the literal's precision",
            "[library][unit][recording-date]")
  {
    auto const fullDate = requireParsed("1981-05-12");
    auto const yearOnly = requireParsed("1981");

    SECTION("report table rows")
    {
      // 1981-05-12 = "1981" is true, so > is false and <= is true.
      CHECK(compareSign(fullDate, requireParsed("1981")) == 0);
      CHECK(compareSign(fullDate, requireParsed("1981-05")) == 0);
      // 1981 = "1981-05" is false and < is true by the zero-component rule.
      CHECK(compareSign(yearOnly, requireParsed("1981-05")) == -1);
      CHECK(compareSign(fullDate, requireParsed("1981-05-11")) == 1);
    }

    SECTION("year-precision bounds of a range")
    {
      // A stored 1950 does not satisfy >= "1950-01-01"...
      CHECK(compareSign(requireParsed("1950"), requireParsed("1950-01-01")) == -1);
      // ...while a stored 1959 satisfies <= "1959-12-31".
      CHECK(compareSign(requireParsed("1959"), requireParsed("1959-12-31")) == -1);
      // A complete date inside 1950..1959 sits above the lower year bound and
      // equals the upper year bound at year precision.
      CHECK(compareSign(requireParsed("1959-11-02"), requireParsed("1950")) == 1);
      CHECK(compareSign(requireParsed("1959-11-02"), requireParsed("1959")) == 0);
    }

    SECTION("differing component values")
    {
      CHECK(compareSign(fullDate, requireParsed("1981-06")) == -1);
      CHECK(compareSign(fullDate, requireParsed("1982")) == -1);
      CHECK(compareSign(yearOnly, requireParsed("1980")) == 1);
      CHECK(compareSign(requireParsed("1981-05-13"), fullDate) == 1);
    }

    SECTION("an absent stored date has no comparison result")
    {
      auto const optOrder = compareRecordingDateAtLiteralPrecision(RecordingDate{}, yearOnly);
      CHECK_FALSE(optOrder);
    }
  }
} // namespace ao::library::test
