// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/query/Completion.h>
#include <ao/query/Field.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/PlanEvaluator.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <string_view>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    constexpr auto kMemberNames =
      std::to_array<std::string_view>({"conductor", "ensemble", "soloist", "performer", "credit"});

    bool matchesCredit(std::string_view expression, TrackFixture& fixture)
    {
      auto const plan = compileOk(parseOk(expression));
      REQUIRE(plan.accessProfile == AccessProfile::ColdOnly);
      auto evaluator = PlanEvaluator{};
      auto cache = library::DictionaryReadCache{fixture.dictionary()};
      auto context = library::DictionaryReadContext{cache};
      auto const binding = PlanBinding{plan, context};
      auto const matches = evaluator.matches(binding, fixture.coldOnlyView());
      CHECK(evaluator.matchesFullPlan(binding, fixture.coldOnlyView()) == matches);
      return matches;
    }
  } // namespace

  TEST_CASE("CreditPredicate - category selectors search every name without crossing kinds", "[query][unit][credit]")
  {
    auto fixture = TrackFixture{TrackSpec{.credits = {
                                            {.name = "First", .kind = CreditKind::Conductor, .role = "Piano"},
                                            {.name = "Second", .kind = CreditKind::Conductor},
                                            {.name = "Second", .kind = CreditKind::Conductor},
                                            {.name = "Orchestra", .kind = CreditKind::Ensemble},
                                            {.name = "Featured", .kind = CreditKind::Soloist},
                                            {.name = "Player", .kind = CreditKind::Performer, .role = "Violin"},
                                          }}};

    for (auto const name : kMemberNames)
    {
      INFO(name);
      CHECK(matchesCredit(std::format("${}?", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${} = 'Piano'", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${} ~ 'Violin'", name), fixture));
    }

    CHECK(matchesCredit("$conductor = 'Second'", fixture));
    CHECK_FALSE(matchesCredit("$conductor != 'Second'", fixture));
    CHECK(matchesCredit("$conductor != 'Orchestra'", fixture));
    CHECK(matchesCredit("$ensemble = 'Orchestra'", fixture));
    CHECK(matchesCredit("$soloist = 'Featured'", fixture));
    CHECK(matchesCredit("$performer = 'Player'", fixture));
    CHECK_FALSE(matchesCredit("$performer = 'Featured'", fixture));
    CHECK_FALSE(matchesCredit("$soloist = 'Player'", fixture));
    CHECK(matchesCredit("$conductor in ['Absent', 'Second']", fixture));
    CHECK(matchesCredit("$conductor in ['Second']", fixture));
    CHECK_FALSE(matchesCredit("$conductor in ['Player', 'Orchestra']", fixture));

    for (auto const* name : {"First", "Second", "Orchestra", "Featured", "Player"})
    {
      CHECK(matchesCredit(std::format("$credit = '{}'", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("$credit != '{}'", name), fixture));
    }
  }

  TEST_CASE("CreditPredicate - empty selections include absence in inequality only", "[query][unit][credit]")
  {
    auto fixture = TrackFixture{};

    for (auto const name : kMemberNames)
    {
      INFO(name);
      CHECK_FALSE(matchesCredit(std::format("${}?", name), fixture));
      CHECK(matchesCredit(std::format("!${}?", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${} = 'Absent'", name), fixture));
      CHECK(matchesCredit(std::format("${} != 'Absent'", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${} ~ ''", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${} in ['Absent', 'Other']", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${}? and ${} != 'Absent'", name, name), fixture));
    }

    auto otherKind = TrackFixture{TrackSpec{.credits = {{.name = "Soloist", .kind = CreditKind::Soloist}}}};
    CHECK_FALSE(matchesCredit("$performer?", otherKind));
    CHECK_FALSE(matchesCredit("$performer ~ ''", otherKind));
    CHECK(matchesCredit("$performer != 'Soloist'", otherKind));
    CHECK(matchesCredit("$credit?", otherKind));
  }

  TEST_CASE("CreditPredicate - equality admits NFC but preserves case and literal whitespace", "[query][unit][credit]")
  {
    auto fixture = TrackFixture{TrackSpec{.credits = {
                                            {.name = "Caf\u00e9", .kind = CreditKind::Conductor},
                                            {.name = "Caf\u00e9", .kind = CreditKind::Ensemble},
                                            {.name = "Caf\u00e9", .kind = CreditKind::Soloist},
                                            {.name = "Caf\u00e9", .kind = CreditKind::Performer},
                                          }}};

    for (auto const name : kMemberNames)
    {
      INFO(name);
      CHECK(matchesCredit(std::format("${} = 'Cafe\u0301'", name), fixture));
      CHECK(matchesCredit(std::format("${} in ['Cafe\u0301', 'Other']", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${} = 'CAF\u00c9'", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${} = ' Caf\u00e9'", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${} = 'Caf\u00e9 '", name), fixture));
      CHECK(matchesCredit(std::format("${} != 'Caf\u00e9 '", name), fixture));
      CHECK_FALSE(matchesCredit(std::format("${} = ''", name), fixture));
    }
  }

  TEST_CASE("CreditPredicate - substring searches individual names with Unicode caseless matching",
            "[query][unit][credit]")
  {
    auto fixture = TrackFixture{TrackSpec{.credits = {
                                            {.name = "Other", .kind = CreditKind::Performer, .role = "Piano"},
                                            {.name = "Stra\u00dfe", .kind = CreditKind::Performer, .role = "Violin"},
                                          }}};
    auto const plan = compileOk(parseOk("$performer ~ 'STRASSE'"));
    CHECK(plan.requiresDictionary);
    CHECK(plan.dictionarySymbols.empty());
    CHECK(matchesCredit("$performer ~ 'STRASSE'", fixture));
    CHECK(matchesCredit("$credit ~ 'STRASSE'", fixture));
    CHECK(matchesCredit("$performer ~ ''", fixture));
    CHECK_FALSE(matchesCredit("$performer ~ 'strasse '", fixture));
    CHECK_FALSE(matchesCredit("$performer ~ 'Violin'", fixture));
    CHECK_FALSE(matchesCredit("$credit ~ 'Other; Stra'", fixture));
  }

  TEST_CASE("CreditPredicate - unresolved constants match nothing without dictionary writes", "[query][unit][credit]")
  {
    auto fixture =
      TrackFixture{TrackSpec{.credits = {{.name = "Glenn Gould", .kind = CreditKind::Performer, .role = "Piano"}}}};
    auto const& dictionary = fixture.dictionary();
    auto const generation = dictionary.generation();
    auto const size = dictionary.size();
    REQUIRE_FALSE(dictionary.findId("Absent Credit"));

    CHECK_FALSE(matchesCredit("$credit = 'Absent Credit'", fixture));
    CHECK(matchesCredit("$credit != 'Absent Credit'", fixture));
    CHECK_FALSE(matchesCredit("$credit in ['Absent Credit']", fixture));
    CHECK_FALSE(matchesCredit("$credit in ['Absent Credit', 'Other']", fixture));
    CHECK(matchesCredit("$credit in ['Absent Credit', 'Glenn Gould']", fixture));
    CHECK_FALSE(dictionary.findId("Absent Credit"));
    CHECK(dictionary.generation() == generation);
    CHECK(dictionary.size() == size);
  }

  TEST_CASE("CreditPredicate - rejects scalar coercion reversed comparisons and indirect operands",
            "[query][unit][credit]")
  {
    for (auto const name : kMemberNames)
    {
      for (auto const* suffix : {" < 'Gould'",
                                 " <= 'Gould'",
                                 " > 'Gould'",
                                 " >= 'Gould'",
                                 " in 'A'..'Z'",
                                 " = 1",
                                 " = false",
                                 " = 1ms",
                                 " ~ 1",
                                 " in ['Gould', 1]",
                                 " in [1]",
                                 " = $title",
                                 " = $soloist",
                                 " = $credit",
                                 " = ('Gould' = 'Gould')",
                                 ""})
      {
        auto const expression = std::format("${}{}", name, suffix);
        INFO(expression);
        CHECK(compileError(parseOk(expression)).code == Error::Code::FormatRejected);
      }

      for (auto const* prefix : {"'Gould' = ", "$title = ", "1 != ", "($year = 2020) = "})
      {
        auto const expression = std::format("{}${}", prefix, name);
        INFO(expression);
        CHECK(compileError(parseOk(expression)).code == Error::Code::FormatRejected);
      }
    }

    CHECK(compileError(parseOk("$musician = 'Gould'")).code == Error::Code::FormatRejected);
    CHECK(compileError(parseOk("$creditRole = 'Piano'")).code == Error::Code::FormatRejected);
    CHECK_FALSE(compileFormat(parseOk("$musician")));
  }

  TEST_CASE("CreditPredicate - members are cold non-scalar fields with dictionary-free existence",
            "[query][unit][credit]")
  {
    for (auto const field : {Field::Conductor, Field::Ensemble, Field::Soloist, Field::Performer, Field::Credit})
    {
      INFO(fieldDisplayName(field));
      CHECK(completeQueryOperator(field, "") == std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
      CHECK(isCreditField(field));
      CHECK_FALSE(isDictionaryField(field));
      CHECK_FALSE(isStringField(field));
      auto const existence = compileOk(parseOk(std::format("${}?", fieldDisplayName(field))));
      CHECK(existence.accessProfile == AccessProfile::ColdOnly);
      CHECK_FALSE(existence.requiresDictionary);
      CHECK(compileOk(parseOk(std::format("${} = 'Name'", fieldDisplayName(field)))).requiresDictionary);
    }

    CHECK(compileOk(parseOk("$credit? and $title = 'Track'")).accessProfile == AccessProfile::HotAndCold);
  }
} // namespace ao::query::test
