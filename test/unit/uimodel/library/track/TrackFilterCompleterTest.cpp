// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/i18n/IcuCompletionAliases.h>
#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/query/Serializer.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/uimodel/library/track/TrackFilter.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    std::vector<std::string> displayTexts(rt::CompletionResult const& result)
    {
      auto values = std::vector<std::string>{};
      values.reserve(result.items.size());

      for (auto const& item : result.items)
      {
        values.push_back(item.displayText);
      }

      return values;
    }

    std::string applyFirst(std::string text, rt::CompletionResult const& result)
    {
      REQUIRE_FALSE(result.items.empty());
      text.replace(result.replaceBegin, result.replaceEnd - result.replaceBegin, result.items.front().insertText);
      return text;
    }
  } // namespace

  TEST_CASE("TrackFilterCompleter - completes every live Quick-filter value kind",
            "[uimodel][unit][track-filter-completion]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
                                                library::test::TrackSpec{.title = "Title Match",
                                                                         .artist = "Artist Match",
                                                                         .album = "Album Match",
                                                                         .albumArtist = "Album Artist Match",
                                                                         .genre = "Genre Match",
                                                                         .composer = "Composer Match",
                                                                         .conductor = "Conductor Match",
                                                                         .work = "Work Match",
                                                                         .tags = {"Tag Match"}});
    auto changes = rt::test::makeStateOnlyLibraryChanges(libraryFixture.library());
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};

    for (auto const& [prefix, expected] : std::vector<std::pair<std::string_view, std::string_view>>{
           {"Title", "Title Match"},
           {"Artist", "Artist Match"},
           {"\"Album M", "Album Match"},
           {"\"Album A", "Album Artist Match"},
           {"Genre", "Genre Match"},
           {"Composer", "Composer Match"},
           {"Work", "Work Match"},
           {"Tag", "Tag Match"},
         })
    {
      auto const optResult = completer.complete(prefix, prefix.size());

      REQUIRE(optResult);
      auto const values = displayTexts(*optResult);
      REQUIRE_FALSE(values.empty());
      CHECK(values.front() == expected);
    }

    CHECK_FALSE(completer.complete("Conductor", std::string_view{"Conductor"}.size()));
  }

  TEST_CASE("TrackFilterCompleter - ranks aggregate matches by live frequency then value",
            "[uimodel][unit][track-filter-completion][ranking]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(),
      library::test::TrackSpec{.title = "Alpine", .artist = "Alpha", .album = "", .tags = {"Alpha"}});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Second", .artist = "Alpha", .album = ""});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Third", .album = "Albatross"});
    auto changes = rt::test::makeStateOnlyLibraryChanges(libraryFixture.library());
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};

    auto const optResult = completer.complete("Al", 2, 2);

    REQUIRE(optResult);
    REQUIRE(displayTexts(*optResult) == std::vector<std::string>{"Alpha", "Albatross"});
    CHECK(optResult->items[0].detail.kind == rt::CompletionDetailKind::Frequency);
    CHECK(optResult->items[0].detail.frequency == 3);
    CHECK(optResult->items[0].rank == 0);
    CHECK(optResult->items[1].detail.kind == rt::CompletionDetailKind::Frequency);
    CHECK(optResult->items[1].detail.frequency == 1);
    CHECK(optResult->items[1].rank == 1);
  }

  TEST_CASE("TrackFilterCompleter - replaces only the current Quick-filter term",
            "[uimodel][unit][track-filter-completion][replacement]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Track", .artist = "Alpha", .album = ""});
    auto changes = rt::test::makeStateOnlyLibraryChanges(libraryFixture.library());
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};
    auto const text = std::string{"road Alzz trip"};
    auto const optResult = completer.complete(text, std::string_view{"road Al"}.size());

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 5);
    CHECK(optResult->replaceEnd == 9);
    CHECK(applyFirst(text, *optResult) == "road \"Alpha\" trip");
  }

  TEST_CASE("TrackFilterCompleter - completes word prefixes without fuzzy correction",
            "[uimodel][unit][track-filter-completion][word-prefix]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "One", .artist = "Trevor Pinnock"});
    auto changes = rt::test::makeStateOnlyLibraryChanges(libraryFixture.library());
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};
    auto const prefix = std::string{"pinnock"};
    auto const optResult = completer.complete(prefix, prefix.size());

    REQUIRE(optResult);
    CHECK(displayTexts(*optResult) == std::vector<std::string>{"Trevor Pinnock"});
    CHECK(applyFirst(prefix, *optResult) == query::serialize(query::ConstantExpression{"Trevor Pinnock"}));
    CHECK_FALSE(completer.complete("innock", std::string_view{"innock"}.size()));
    CHECK_FALSE(completer.complete("pinnok", std::string_view{"pinnok"}.size()));
  }

  TEST_CASE("TrackFilterCompleter - quotes inserted values for lossless Quick-filter resolution",
            "[uimodel][unit][track-filter-completion][escaping]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto const value = std::string{R"(C:\Music "Live")"};
    library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(), library::test::TrackSpec{.title = value});
    auto changes = rt::test::makeStateOnlyLibraryChanges(libraryFixture.library());
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};
    auto const optResult = completer.complete("C:", 2);

    REQUIRE(optResult);
    auto const completed = applyFirst("C:", *optResult);
    CHECK(completed == query::serialize(query::ConstantExpression{value}));

    auto const resolved = resolveTrackFilter(completed);
    CHECK(resolved.mode == TrackFilterMode::Quick);
    CHECK(resolved.expression.contains(query::serialize(query::ConstantExpression{value})));
    CHECK(query::parse(resolved.expression).has_value());
  }

  TEST_CASE("TrackFilterCompleter - uses the explicit expression boundary shared with the resolver",
            "[uimodel][unit][track-filter-completion][mode]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "P!nk Live", .artist = "P!nk"});
    auto changes = rt::test::makeStateOnlyLibraryChanges(libraryFixture.library());
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};

    auto optResult = completer.complete("P!n", 3);
    REQUIRE(optResult);
    auto const values = displayTexts(*optResult);
    REQUIRE_FALSE(values.empty());
    CHECK(values.front() == "P!nk");
    CHECK(resolveTrackFilter("P!nk").mode == TrackFilterMode::Quick);

    optResult = completer.complete("$ar", 3);
    REQUIRE(optResult);
    REQUIRE_FALSE(optResult->items.empty());
    CHECK(optResult->items.front().insertText == "$artist");
    CHECK(resolveTrackFilter("$artist = \"P!nk\"").mode == TrackFilterMode::Expression);

    CHECK_FALSE(completer.complete("", 0));
    CHECK_FALSE(completer.complete("road ", 5));
  }

  TEST_CASE("TrackFilterCompleter - romanized completion remains an original-text selection aid",
            "[uimodel][unit][completion-alias][track-filter-completion]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "One", .artist = "周杰倫"});
    auto changes = rt::test::makeStateOnlyLibraryChanges(libraryFixture.library());
    auto aliasPolicyPtr = i18n::createIcuCompletionAliasPolicy();
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes, nullptr, aliasPolicyPtr.get()};
    auto completer = TrackFilterCompleter{vocabulary};
    auto const optResult = completer.complete("zhoujielun", std::string_view{"zhoujielun"}.size());

    REQUIRE(optResult);
    CHECK(displayTexts(*optResult) == std::vector<std::string>{"周杰倫"});
    auto const completed = applyFirst("zhoujielun", *optResult);
    CHECK(completed == query::serialize(query::ConstantExpression{"周杰倫"}));
    CHECK(query::parse(resolveTrackFilter(completed).expression).has_value());

    auto const unresolved = resolveTrackFilter("zhoujielun");
    CHECK(unresolved.mode == TrackFilterMode::Quick);
    CHECK_FALSE(unresolved.expression.contains("周杰倫"));
    CHECK_FALSE(completer.complete("周abc", std::string_view{"周abc"}.size()));
  }

  TEST_CASE("TrackFilterCompleter - direct matches outrank aliases and alias collisions remain distinct",
            "[uimodel][unit][completion-alias][track-filter-completion]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "One", .artist = "周杰倫"});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Two", .artist = "周杰倫"});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Three", .artist = "Zhou Direct"});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Four", .artist = "王菲"});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Five", .artist = "王妃"});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Six", .artist = "宇多田ヒカル"});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Seven", .artist = "久石譲"});
    auto changes = rt::test::makeStateOnlyLibraryChanges(libraryFixture.library());
    auto aliasPolicyPtr = i18n::createIcuCompletionAliasPolicy();
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes, nullptr, aliasPolicyPtr.get()};
    auto completer = TrackFilterCompleter{vocabulary};

    auto optResult = completer.complete("zhou", 4, 1);
    REQUIRE(optResult);
    CHECK(displayTexts(*optResult) == std::vector<std::string>{"Zhou Direct"});

    optResult = completer.complete("wangfei", 7);
    REQUIRE(optResult);
    CHECK(displayTexts(*optResult) == std::vector<std::string>{"王妃", "王菲"});

    optResult = completer.complete("hikaru", 6);
    REQUIRE(optResult);
    CHECK(displayTexts(*optResult) == std::vector<std::string>{"宇多田ヒカル"});

    CHECK_FALSE(completer.complete("hisaishijoe", 11));
    optResult = completer.complete("jiushirang", 10);
    REQUIRE(optResult);
    CHECK(displayTexts(*optResult) == std::vector<std::string>{"久石譲"});
  }
} // namespace ao::uimodel::test
