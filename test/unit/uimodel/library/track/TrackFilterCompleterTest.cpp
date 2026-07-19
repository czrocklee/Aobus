// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/query/Serializer.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/library/track/TrackFilterCompleter.h>
#include <ao/uimodel/library/track/TrackFilterResolver.h>

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
    library::test::addTrack(libraryFixture.library(),
                            library::test::TrackSpec{.title = "Title Match",
                                                     .artist = "Artist Match",
                                                     .album = "Album Match",
                                                     .albumArtist = "Album Artist Match",
                                                     .genre = "Genre Match",
                                                     .composer = "Composer Match",
                                                     .conductor = "Conductor Match",
                                                     .work = "Work Match",
                                                     .tags = {"Tag Match"}});
    auto changes = rt::LibraryChanges{};
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
      CHECK(displayTexts(*optResult).front() == expected);
    }

    CHECK_FALSE(completer.complete("Conductor", std::string_view{"Conductor"}.size()));
  }

  TEST_CASE("TrackFilterCompleter - ranks aggregate matches by live frequency then value",
            "[uimodel][unit][track-filter-completion][ranking]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrack(
      libraryFixture.library(),
      library::test::TrackSpec{.title = "Alpine", .artist = "Alpha", .album = "", .tags = {"Alpha"}});
    library::test::addTrack(
      libraryFixture.library(), library::test::TrackSpec{.title = "Second", .artist = "Alpha", .album = ""});
    library::test::addTrack(libraryFixture.library(), library::test::TrackSpec{.title = "Third", .album = "Albatross"});
    auto changes = rt::LibraryChanges{};
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};

    auto const optResult = completer.complete("Al", 2, 2);

    REQUIRE(optResult);
    CHECK(displayTexts(*optResult) == std::vector<std::string>{"Alpha", "Albatross"});
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
    library::test::addTrack(
      libraryFixture.library(), library::test::TrackSpec{.title = "Track", .artist = "Alpha", .album = ""});
    auto changes = rt::LibraryChanges{};
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};
    auto const text = std::string{"road Alzz trip"};
    auto const optResult = completer.complete(text, std::string_view{"road Al"}.size());

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == 5);
    CHECK(optResult->replaceEnd == 9);
    CHECK(applyFirst(text, *optResult) == "road \"Alpha\" trip");
  }

  TEST_CASE("TrackFilterCompleter - quotes inserted values for lossless Quick-filter resolution",
            "[uimodel][unit][track-filter-completion][escaping]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto const value = std::string{R"(C:\Music "Live")"};
    library::test::addTrack(libraryFixture.library(), library::test::TrackSpec{.title = value});
    auto changes = rt::LibraryChanges{};
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};
    auto const optResult = completer.complete("C:", 2);

    REQUIRE(optResult);
    auto const completed = applyFirst("C:", *optResult);
    CHECK(completed == query::serialize(query::ConstantExpression{value}));

    auto const resolved = resolveTrackFilterExpression(completed);
    CHECK(resolved.mode == TrackFilterMode::Quick);
    CHECK(resolved.expression.contains(query::serialize(query::ConstantExpression{value})));
    CHECK(query::parse(resolved.expression).has_value());
  }

  TEST_CASE("TrackFilterCompleter - uses the explicit expression boundary shared with the resolver",
            "[uimodel][unit][track-filter-completion][mode]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    library::test::addTrack(libraryFixture.library(), library::test::TrackSpec{.title = "P!nk Live", .artist = "P!nk"});
    auto changes = rt::LibraryChanges{};
    auto vocabulary = rt::CompletionService{libraryFixture.library(), changes};
    auto completer = TrackFilterCompleter{vocabulary};

    auto optResult = completer.complete("P!n", 3);
    REQUIRE(optResult);
    CHECK(displayTexts(*optResult).front() == "P!nk");
    CHECK(resolveTrackFilterExpression("P!nk").mode == TrackFilterMode::Quick);

    optResult = completer.complete("$ar", 3);
    REQUIRE(optResult);
    CHECK(optResult->items.front().insertText == "$artist");
    CHECK(resolveTrackFilterExpression("$artist = \"P!nk\"").mode == TrackFilterMode::Expression);

    CHECK_FALSE(completer.complete("", 0));
    CHECK_FALSE(completer.complete("road ", 5));
  }
} // namespace ao::uimodel::test
