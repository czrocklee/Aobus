// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/Error.h>
#include <ao/library/RecordingDate.h>
#include <ao/query/Completion.h>
#include <ao/query/Field.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompilation.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    bool matchesDate(std::string_view expression, TrackFixture const& fixture)
    {
      auto const plan = compileOk(parseOk(expression));
      REQUIRE(plan.accessProfile == AccessProfile::ColdOnly);
      REQUIRE_FALSE(plan.requiresDictionary);
      auto evaluator = PlanEvaluator{};
      return evaluator.matches(plan, fixture.coldOnlyView());
    }
  } // namespace

  TEST_CASE("RecordingDatePredicate - compares every operator at literal precision", "[query][unit][recording-date]")
  {
    auto fixture = TrackFixture{TrackSpec{.recordingDate = {.year = 1981, .month = 5, .day = 12}}};

    CHECK(matchesDate("$recordingDate?", fixture));
    CHECK(matchesDate("$recordingDate = 1981", fixture));
    CHECK(matchesDate("$recordingDate = '1981'", fixture));
    CHECK(matchesDate("$recordingDate = '1981-05'", fixture));
    CHECK(matchesDate("$recordingDate = '1981-05-12'", fixture));
    CHECK_FALSE(matchesDate("$recordingDate != '1981-05'", fixture));
    CHECK_FALSE(matchesDate("$recordingDate > '1981'", fixture));
    CHECK(matchesDate("$recordingDate <= '1981'", fixture));
    CHECK(matchesDate("$recordingDate >= '1981-05'", fixture));
    CHECK(matchesDate("$recordingDate > '1981-05-11'", fixture));
    CHECK_FALSE(matchesDate("$recordingDate < '1981-05-12'", fixture));
    CHECK(matchesDate("$recordingDate < '1981-05-13'", fixture));
    CHECK(matchesDate("$recordingDate != 1955", fixture));
    CHECK(matchesDate("($recordingDate = 1981) and true", fixture));
    CHECK(matchesDate("($recordingDate = 1981) = true", fixture));
    CHECK(matchesDate("($recordingDate = 1955) = false", fixture));
  }

  TEST_CASE("RecordingDatePredicate - keeps unknown stored components zero", "[query][unit][recording-date]")
  {
    auto fixture = TrackFixture{TrackSpec{.recordingDate = {.year = 1981}}};

    CHECK(matchesDate("$recordingDate = 1981", fixture));
    CHECK_FALSE(matchesDate("$recordingDate = '1981-05'", fixture));
    CHECK(matchesDate("$recordingDate < '1981-05'", fixture));
    CHECK_FALSE(matchesDate("$recordingDate >= '1981-01-01'", fixture));
    CHECK(matchesDate("$recordingDate <= '1981-12-31'", fixture));

    auto month = TrackFixture{TrackSpec{.recordingDate = {.year = 1981, .month = 5}}};
    CHECK(matchesDate("$recordingDate = '1981-05'", month));
    CHECK_FALSE(matchesDate("$recordingDate = '1981-05-12'", month));
    CHECK(matchesDate("$recordingDate < '1981-05-01'", month));
  }

  TEST_CASE("RecordingDatePredicate - lists and each range bound retain their precision",
            "[query][unit][recording-date]")
  {
    auto fixture = TrackFixture{TrackSpec{.recordingDate = {.year = 1959, .month = 11, .day = 2}}};

    CHECK(matchesDate("$recordingDate in 1950..1959", fixture));
    CHECK(matchesDate("$recordingDate in '1950-01-01'..1959", fixture));
    CHECK_FALSE(matchesDate("$recordingDate in 1950..'1959-11-01'", fixture));
    CHECK(matchesDate("$recordingDate in [1955, '1959-11']", fixture));
    CHECK_FALSE(matchesDate("$recordingDate in ['1959-10', 1981]", fixture));

    auto coarse = TrackFixture{TrackSpec{.recordingDate = {.year = 1950}}};
    CHECK(matchesDate("$recordingDate in 1950..1959", coarse));
    CHECK_FALSE(matchesDate("$recordingDate in '1950-01-01'..1959", coarse));
    CHECK(matchesDate("$recordingDate in ['1950-01', 1950]", coarse));
  }

  TEST_CASE("RecordingDatePredicate - absent dates only match missing and inequality", "[query][unit][recording-date]")
  {
    // The ordinary hot year is present; it must never supply a recording date.
    auto fixture = TrackFixture{TrackSpec{.year = 1981}};

    CHECK_FALSE(matchesDate("$recordingDate?", fixture));
    CHECK(matchesDate("!$recordingDate?", fixture));
    CHECK(matchesDate("$recordingDate != 1981", fixture));
    CHECK_FALSE(matchesDate("$recordingDate? and $recordingDate != 1981", fixture));

    for (auto const* const expression : {"$recordingDate = 1981",
                                         "$recordingDate < 1981",
                                         "$recordingDate <= 1981",
                                         "$recordingDate > 1981",
                                         "$recordingDate >= 1981",
                                         "$recordingDate in [1955, 1981]",
                                         "$recordingDate in 1950..1990"})
    {
      INFO(expression);
      CHECK_FALSE(matchesDate(expression, fixture));
    }
  }

  TEST_CASE("RecordingDatePredicate - rejects noncanonical literals and scalar conversions",
            "[query][unit][recording-date]")
  {
    for (auto const* const expression : {"$recordingDate = 0",
                                         "$recordingDate = -1",
                                         "$recordingDate = 10000",
                                         "$recordingDate = true",
                                         "$recordingDate = 1981ms",
                                         "$recordingDate = ''",
                                         "$recordingDate = '0000'",
                                         "$recordingDate = '1981-5'",
                                         "$recordingDate = '1981-00'",
                                         "$recordingDate = '1981-13'",
                                         "$recordingDate = '1900-02-29'",
                                         "$recordingDate = '1981-04-31'",
                                         "$recordingDate = ' 1981'",
                                         "$recordingDate = '1981 '",
                                         "$recordingDate = '１９８１'",
                                         "$recordingDate = '1981-05-12T00:00:00'",
                                         "$recordingDate ~ '1981'",
                                         "$recordingDate = $year",
                                         "$recordingDate = $recordingDate",
                                         "$year = $recordingDate",
                                         "1981 = $recordingDate",
                                         "$recordingDate in [1981, false]",
                                         "$recordingDate in 0..1981",
                                         "$recordingDate"})
    {
      INFO(expression);
      auto const error = compileError(parseOk(expression));
      CHECK(error.code == Error::Code::FormatRejected);
    }

    for (auto const* const expression : {"$recordingDate = 1981-05", "$recordingDate = 1981-05-12"})
    {
      INFO(expression);

      if (auto const parseRes = parse(expression); parseRes)
      {
        auto const compileRes = compileQuery(*parseRes);
        REQUIRE_FALSE(compileRes);
        CHECK(compileRes.error().code == Error::Code::FormatRejected);
      }
      else
      {
        CHECK(parseRes.error().code == Error::Code::FormatRejected);
      }
    }

    CHECK(compileOk(parseOk("$recordingDate = '2000-02-29'")).recordingDateConstants[0] ==
          library::RecordingDate{.year = 2000, .month = 2, .day = 29});
    CHECK(compileOk(parseOk("$recordingDate = '0001'")).recordingDateConstants[0].year == 1);
    CHECK(compileOk(parseOk("$recordingDate = 9999")).recordingDateConstants[0].year == 9999);
  }

  TEST_CASE("RecordingDatePredicate - catalog completion and mixed access remain typed",
            "[query][unit][recording-date]")
  {
    CHECK(completeQueryOperator(Field::RecordingDate, "") ==
          std::vector<std::string_view>{"=", "!=", "<", "<=", ">", ">=", "in", "?"});
    CHECK(compileOk(parseOk("$recordingDate? and $year = 1981")).accessProfile == AccessProfile::HotAndCold);
    CHECK_FALSE(isDictionaryField(Field::RecordingDate));
    CHECK_FALSE(isStringField(Field::RecordingDate));
  }

  TEST_CASE("FormatExpression - recording dates preserve precision and absence", "[query][unit][recording-date]")
  {
    auto const formatRes = compileFormat(parseOk("'[' + $recordingDate + ']'"));
    REQUIRE(formatRes);
    CHECK(formatRes->accessProfile == AccessProfile::ColdOnly);
    CHECK_FALSE(formatRes->requiresDictionary);
    auto const binding = FormatBinding{*formatRes};
    auto evaluator = FormatEvaluator{};
    auto output = std::string{};

    struct Case final
    {
      library::RecordingDate date;
      std::string_view expected;
    };

    for (auto const& value : {Case{.date = {}, .expected = "[]"},
                              Case{.date = {.year = 1981}, .expected = "[1981]"},
                              Case{.date = {.year = 1981, .month = 5}, .expected = "[1981-05]"},
                              Case{.date = {.year = 1981, .month = 5, .day = 12}, .expected = "[1981-05-12]"}})
    {
      auto fixture = TrackFixture{TrackSpec{.recordingDate = value.date}};
      evaluator.evaluate(binding, fixture.coldOnlyView(), output);
      CHECK(output == value.expected);
    }
  }
} // namespace ao::query::test
