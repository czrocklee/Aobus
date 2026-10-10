// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/completion/CompletionTestSupport.h"
#include <ao/i18n/IcuCompletionAliases.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    using Kind = library::CreditKind;

    std::vector<std::pair<std::string, std::uint32_t>> creditVocabulary(std::span<VocabularyEntry const> entries)
    {
      auto values = std::vector<std::pair<std::string, std::uint32_t>>{};

      for (auto const& entry : entries)
      {
        values.emplace_back(entry.value, entry.frequency);
      }

      return values;
    }
  } // namespace

  TEST_CASE("CompletionService - credits count every name once per track globally and per kind",
            "[runtime][unit][completion][credits]")
  {
    auto fixture = MusicLibraryFixture{};
    std::ignore = fixture.addTrack(library::test::TrackSpec{
      .title = "First",
      .credits = {{.name = " Ada ", .kind = Kind::Conductor, .role = " Piano "},
                  {.name = "Ada", .kind = Kind::Conductor, .role = "Piano"},
                  {.name = "Bob", .kind = Kind::Conductor, .role = "Violin"},
                  {.name = "Ada", .kind = Kind::Ensemble},
                  {.name = "Featured", .kind = Kind::Soloist},
                  {.name = "Bob", .kind = Kind::Performer, .role = "Violin"}},
    });
    std::ignore = fixture.addTrack(library::test::TrackSpec{
      .title = "Second",
      .credits = {{.name = "Bob", .kind = Kind::Ensemble, .role = "Violin"}, {.name = "Cara", .role = " \t "}},
    });
    auto changes = makeStateOnlyLibraryChanges(fixture.library());
    auto service = CompletionService{fixture.library(), changes};

    CHECK(creditVocabulary(service.creditNames()) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Bob", 2}, {"Ada", 1}, {"Cara", 1}, {"Featured", 1}});
    CHECK(creditVocabulary(service.creditRoles()) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Violin", 2}, {"Piano", 1}});
    CHECK(creditVocabulary(service.creditNames(Kind::Conductor)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Ada", 1}, {"Bob", 1}});
    CHECK(creditVocabulary(service.creditNames(Kind::Ensemble)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Ada", 1}, {"Bob", 1}});
    CHECK(creditVocabulary(service.creditNames(Kind::Soloist)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Featured", 1}});
    CHECK(creditVocabulary(service.creditNames(Kind::Performer)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Bob", 1}, {"Cara", 1}});
    CHECK(service.valuesFor(TrackField::Conductor).data() == service.creditNames(Kind::Conductor).data());
    CHECK(service.valuesFor(TrackField::Ensemble).data() == service.creditNames(Kind::Ensemble).data());
    CHECK(service.valuesFor(TrackField::Soloist).data() == service.creditNames(Kind::Soloist).data());
    constexpr auto kConductorFields = std::array{TrackField::Conductor};
    auto aggregate = creditVocabulary(service.aggregateValues({.fields = kConductorFields}));
    std::ranges::sort(aggregate);
    CHECK(aggregate == std::vector<std::pair<std::string, std::uint32_t>>{{"Ada", 1}, {"Bob", 1}});
  }

  TEST_CASE("CompletionService - nonfirst and role-only replacements invalidate credit vocabularies",
            "[runtime][unit][completion][credits]")
  {
    auto fixture = MusicLibraryFixture{};
    auto const trackId = fixture.addTrack(library::test::TrackSpec{
      .title = "First",
      .credits = {{.name = "Ada", .kind = Kind::Conductor}, {.name = "Bob", .kind = Kind::Conductor, .role = "Piano"}},
    });
    auto changes = makeStateOnlyLibraryChanges(fixture.library());
    auto commands = LibraryCommandsFixture{fixture.library(), changes};
    auto service = CompletionService{fixture.library(), changes};
    REQUIRE(creditVocabulary(service.creditNames()) ==
            std::vector<std::pair<std::string, std::uint32_t>>{{"Ada", 1}, {"Bob", 1}});
    REQUIRE(creditVocabulary(service.creditRoles()) ==
            std::vector<std::pair<std::string, std::uint32_t>>{{"Piano", 1}});
    REQUIRE(service.creditNames(Kind::Conductor).size() == 2);
    auto replacement = CreditReplacement{.kinds = 1,
                                         .entries = {{.name = "Ada", .kind = Kind::Conductor},
                                                     {.name = "Cara", .kind = Kind::Conductor, .role = "Piano"}}};

    REQUIRE(commands.updateMetadata(std::array{trackId}, MetadataPatch{.optCredits = replacement}));
    CHECK(creditVocabulary(service.creditNames()) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Ada", 1}, {"Cara", 1}});
    CHECK(creditVocabulary(service.creditNames(Kind::Conductor)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Ada", 1}, {"Cara", 1}});
    replacement.entries.back().role = "Violin";
    REQUIRE(commands.updateMetadata(std::array{trackId}, MetadataPatch{.optCredits = replacement}));
    CHECK(creditVocabulary(service.creditRoles()) == std::vector<std::pair<std::string, std::uint32_t>>{{"Violin", 1}});
    CHECK(creditVocabulary(service.creditNames()) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Ada", 1}, {"Cara", 1}});
    REQUIRE(commands.updateMetadata(std::array{trackId}, MetadataPatch{.optCredits = CreditReplacement{.kinds = 1}}));
    CHECK(service.creditNames().empty());
    CHECK(service.creditRoles().empty());
    CHECK(service.creditNames(Kind::Conductor).empty());
  }

  TEST_CASE("Credit completion - editor and member-query providers select names by kind and roles globally",
            "[runtime][unit][completion][credits]")
  {
    auto fixture = MusicLibraryFixture{};
    std::ignore = fixture.addTrack(library::test::TrackSpec{
      .title = "First",
      .credits = {{.name = "Ada", .kind = Kind::Conductor},
                  {.name = "Bob", .kind = Kind::Conductor},
                  {.name = "Band", .kind = Kind::Ensemble},
                  {.name = "Featured", .kind = Kind::Soloist},
                  {.name = "Player", .role = "Violin"}},
    });
    auto changes = makeStateOnlyLibraryChanges(fixture.library());
    auto service = CompletionService{fixture.library(), changes};
    auto nameProvider = makeCreditNameCompletionProvider(service, Kind::Conductor);
    auto roleProvider = makeCreditRoleCompletionProvider(service);
    auto const optName = nameProvider("bo suffix", 2);
    REQUIRE(optName);
    CHECK(optName->replaceBegin == 0);
    CHECK(optName->replaceEnd == std::string{"bo suffix"}.size());
    CHECK(insertTexts(optName->items) == std::vector<std::string>{"Bob"});
    auto const optRole = roleProvider("vio", 100);
    REQUIRE(optRole);
    CHECK(optRole->replaceBegin == 0);
    CHECK(optRole->replaceEnd == 3);
    CHECK(insertTexts(optRole->items) == std::vector<std::string>{"Violin"});
    CHECK_FALSE(nameProvider("vio", 3));
    CHECK_FALSE(nameProvider("pla", 3));
    CHECK_FALSE(roleProvider("bo", 2));
    auto performerProvider = makeCreditNameCompletionProvider(service, Kind::Performer);
    REQUIRE(performerProvider("pla", 3));
    CHECK(insertTexts(performerProvider("pla", 3)->items) == std::vector<std::string>{"Player"});
    CHECK_FALSE(performerProvider("bo", 2));
    auto queryCompleter = QueryExpressionCompleter{service};

    for (auto const& [expression, expected] :
         std::array{std::pair{std::string{"$credit in [bo"}, std::string{R"("Bob")"}},
                    std::pair{std::string{"$conductor = bo"}, std::string{R"("Bob")"}},
                    std::pair{std::string{"$ensemble = ba"}, std::string{R"("Band")"}},
                    std::pair{std::string{"$soloist = fe"}, std::string{R"("Featured")"}},
                    std::pair{std::string{"$performer = pl"}, std::string{R"("Player")"}}})
    {
      auto const optQuery = queryCompleter.complete(expression, expression.size());
      REQUIRE(optQuery);
      CHECK(optQuery->replaceBegin == expression.size() - 2);
      CHECK(optQuery->replaceEnd == expression.size());
      CHECK(insertTexts(optQuery->items) == std::vector<std::string>{expected});
    }

    for (auto const& expression : {std::string{"$credit in [vio"},
                                   std::string{"$performer = bo"},
                                   std::string{"$conductor = pl"},
                                   std::string{"$musician = bo"}})
    {
      CHECK_FALSE(queryCompleter.complete(expression, expression.size()));
    }
  }

  TEST_CASE("Credit completion - aliases survive category and role materialization and invalidation",
            "[runtime][unit][completion][credits][completion-alias]")
  {
    auto fixture = MusicLibraryFixture{};
    auto const trackId = fixture.addTrack(library::test::TrackSpec{
      .title = "First",
      .credits = {{.name = "周杰倫", .role = "周杰倫"}},
    });
    auto changes = makeStateOnlyLibraryChanges(fixture.library());
    auto commands = LibraryCommandsFixture{fixture.library(), changes};
    auto aliasesPtr = i18n::createIcuCompletionAliasPolicy();
    auto service = CompletionService{fixture.library(), changes, nullptr, aliasesPtr.get()};
    auto nameProvider = makeCreditNameCompletionProvider(service, Kind::Performer);
    auto roleProvider = makeCreditRoleCompletionProvider(service);
    auto const optName = nameProvider("zhoujielun", 10);
    REQUIRE(optName);
    CHECK(insertTexts(optName->items) == std::vector<std::string>{"周杰倫"});
    auto const names = service.creditNames();
    REQUIRE(names.size() == 1);
    auto const optRole = roleProvider("zhoujielun", 10);
    REQUIRE(optRole);
    CHECK(insertTexts(optRole->items) == std::vector<std::string>{"周杰倫"});
    CHECK_FALSE(names.front().aliases.empty());
    auto const optNameAfterRoles = nameProvider("zhoujielun", 10);
    REQUIRE(optNameAfterRoles);
    CHECK(insertTexts(optNameAfterRoles->items) == std::vector<std::string>{"周杰倫"});
    auto const replacement = CreditReplacement{.kinds = 8, .entries = {{.name = "王菲", .role = "Piano"}}};
    REQUIRE(commands.updateMetadata(std::array{trackId}, MetadataPatch{.optCredits = replacement}));
    CHECK_FALSE(nameProvider("zhoujielun", 10));
    auto const optUpdated = nameProvider("wangfei", 7);
    REQUIRE(optUpdated);
    CHECK(insertTexts(optUpdated->items) == std::vector<std::string>{"王菲"});
    CHECK_FALSE(roleProvider("zhoujielun", 10));
  }

  TEST_CASE("CompletionService - live locale changes reorder global and category credit vocabulary ties",
            "[runtime][unit][completion][credits][collation]")
  {
    auto fixture = MusicLibraryFixture{};
    auto entries = std::vector<library::Credit>{};

    for (auto const kind : {Kind::Conductor, Kind::Ensemble, Kind::Soloist, Kind::Performer})
    {
      entries.push_back({.name = "ä", .kind = kind, .role = "ä"});
      entries.push_back({.name = "z", .kind = kind, .role = "z"});
    }

    std::ignore = fixture.addTrack(library::test::TrackSpec{.title = "First", .credits = entries});
    auto changes = makeStateOnlyLibraryChanges(fixture.library());
    auto service = CompletionService{fixture.library(), changes};
    auto germanRes = i18n::createIcuTextOrderingPolicy("de-DE");
    auto swedishRes = i18n::createIcuTextOrderingPolicy("sv-SE");
    REQUIRE(germanRes);
    REQUIRE(swedishRes);
    service.setTextOrderingPolicy(std::shared_ptr<TextOrderingPolicy const>{std::move(*germanRes)});
    CHECK(creditVocabulary(service.creditNames()) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"ä", 1}, {"z", 1}});
    CHECK(creditVocabulary(service.creditRoles()) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"ä", 1}, {"z", 1}});

    for (auto const kind : {Kind::Conductor, Kind::Ensemble, Kind::Soloist, Kind::Performer})
    {
      CHECK(creditVocabulary(service.creditNames(kind)) ==
            std::vector<std::pair<std::string, std::uint32_t>>{{"ä", 1}, {"z", 1}});
    }

    service.setTextOrderingPolicy(std::shared_ptr<TextOrderingPolicy const>{std::move(*swedishRes)});
    CHECK(creditVocabulary(service.creditNames()) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"z", 1}, {"ä", 1}});
    CHECK(creditVocabulary(service.creditRoles()) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"z", 1}, {"ä", 1}});

    for (auto const kind : {Kind::Conductor, Kind::Ensemble, Kind::Soloist, Kind::Performer})
    {
      CHECK(creditVocabulary(service.creditNames(kind)) ==
            std::vector<std::pair<std::string, std::uint32_t>>{{"z", 1}, {"ä", 1}});
    }
  }
} // namespace ao::rt::test
