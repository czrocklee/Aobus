// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/completion/QueryExpressionCompleter.h>

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/completion/CompletionTestSupport.h"
#include <ao/i18n/IcuCompletionAliases.h>
#include <ao/rt/completion/CompletionAliasPolicy.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryChanges.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void addCompletionTrack(MusicLibraryFixture& libraryFixture,
                            std::span<std::string const> tags,
                            std::span<std::pair<std::string, std::string> const> custom)
    {
      library::test::addTrackWithUniqueFixtureUri(
        libraryFixture.library(),
        library::test::TrackSpec{.title = "Completion Track",
                                 .artist = "Artist",
                                 .album = "Album",
                                 .conductor = "Conductor",
                                 .ensemble = "Ensemble",
                                 .soloist = "Soloist",
                                 .uri = "query-completion.flac",
                                 .tags = {tags.begin(), tags.end()},
                                 .customMetadata = {custom.begin(), custom.end()},
                                 .duration = std::chrono::seconds{120}});
    }

    QueryExpressionCompleter makeCompleter(MusicLibraryFixture& libraryFixture,
                                           std::unique_ptr<LibraryChanges>& changesPtr,
                                           std::unique_ptr<CompletionService>& servicePtr,
                                           CompletionAliasPolicy const* completionAliasPolicy = nullptr)
    {
      static thread_local auto executor = InlineExecutor{};
      auto const transaction = libraryFixture.library().readTransaction();
      changesPtr = std::make_unique<LibraryChanges>(
        executor, libraryFixture.library().libraryRevision(transaction), "test-library");
      servicePtr =
        std::make_unique<CompletionService>(libraryFixture.library(), *changesPtr, nullptr, completionAliasPolicy);
      return QueryExpressionCompleter{*servicePtr};
    }
  } // namespace

  TEST_CASE("QueryExpressionCompleter - completes field aliases from query prefixes",
            "[runtime][unit][completion-query]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(libraryFixture, changesPtr, servicePtr);

    auto optAlbum = completer.complete("$al", 3);
    REQUIRE(optAlbum);
    CHECK(optAlbum->replaceBegin == 0);
    CHECK(optAlbum->replaceEnd == 3);
    REQUIRE(insertTexts(optAlbum->items) == std::vector<std::string>{"$album", "$albumArtist"});
    checkCompletionItem(optAlbum->items[0], "$album", "$album", CompletionDetailKind::Alias, 0, 0);
    checkCompletionItem(optAlbum->items[1], "$albumArtist", "$albumArtist", CompletionDetailKind::Field, 0, 1);

    auto optTrackNumber = completer.complete("$tn", 3);
    REQUIRE(optTrackNumber);
    CHECK(insertTexts(optTrackNumber->items) == std::vector<std::string>{"$trackNumber"});

    auto optBitrate = completer.complete("@BR", 3);
    REQUIRE(optBitrate);
    CHECK(insertTexts(optBitrate->items) == std::vector<std::string>{"@bitrate"});
  }

  TEST_CASE("QueryExpressionCompleter - completes operators allowed after fields",
            "[runtime][unit][completion-query][operator]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(libraryFixture, changesPtr, servicePtr);

    auto optArtist = completer.complete("$artist ", 8);
    REQUIRE(optArtist);
    CHECK(optArtist->replaceBegin == 7);
    CHECK(optArtist->replaceEnd == 8);
    REQUIRE(insertTexts(optArtist->items) == std::vector<std::string>{" = ", " != ", " ~ ", " in ", "?"});
    checkCompletionItem(optArtist->items[0], "=", " = ", CompletionDetailKind::Operator, 0, 0);
    checkCompletionItem(optArtist->items[1], "!=", " != ", CompletionDetailKind::Operator, 0, 1);

    auto optYear = completer.complete("$year >", 7);
    REQUIRE(optYear);
    CHECK(optYear->replaceBegin == 5);
    CHECK(optYear->replaceEnd == 7);
    CHECK(insertTexts(optYear->items) == std::vector<std::string>{" > ", " >= "});
  }

  TEST_CASE("QueryExpressionCompleter - completes logical operators after values",
            "[runtime][unit][completion-query][operator]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(libraryFixture, changesPtr, servicePtr);

    auto optAfterValue = completer.complete(R"($artist = "Miles" )", 18);
    REQUIRE(optAfterValue);
    CHECK(optAfterValue->replaceBegin == 17);
    CHECK(optAfterValue->replaceEnd == 18);
    REQUIRE(insertTexts(optAfterValue->items) == std::vector<std::string>{" and ", " or ", " && ", " || "});
    checkCompletionItem(optAfterValue->items[0], "and", " and ", CompletionDetailKind::LogicalOperator, 0, 0);
    checkCompletionItem(optAfterValue->items[1], "or", " or ", CompletionDetailKind::LogicalOperator, 0, 1);

    auto optAnd = completer.complete(R"($artist = "Miles" a)", 19);
    REQUIRE(optAnd);
    CHECK(optAnd->replaceBegin == 17);
    CHECK(optAnd->replaceEnd == 19);
    CHECK(insertTexts(optAnd->items) == std::vector<std::string>{" and "});

    auto optSymbol = completer.complete("$year >= 1999 |", 15);
    REQUIRE(optSymbol);
    CHECK(optSymbol->replaceBegin == 13);
    CHECK(optSymbol->replaceEnd == 15);
    CHECK(insertTexts(optSymbol->items) == std::vector<std::string>{" || "});

    auto optTag = completer.complete("#rock ", 6);
    REQUIRE(optTag);
    CHECK(optTag->replaceBegin == 5);
    CHECK(optTag->replaceEnd == 6);
    CHECK(insertTexts(optTag->items) == std::vector<std::string>{" and ", " or ", " && ", " || "});
  }

  TEST_CASE("QueryExpressionCompleter - completes metadata values for value positions",
            "[runtime][unit][completion-query][value]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto tags = std::vector<std::string>{};
    auto custom = std::vector<std::pair<std::string, std::string>>{};
    addCompletionTrack(libraryFixture, tags, custom);
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Second", .artist = "Trevor Pinnock"});

    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(libraryFixture, changesPtr, servicePtr);

    auto optArtist = completer.complete("$artist = Ar", 12);
    REQUIRE(optArtist);
    CHECK(optArtist->replaceBegin == 10);
    CHECK(optArtist->replaceEnd == 12);
    REQUIRE(insertTexts(optArtist->items) == std::vector<std::string>{R"("Artist")"});
    checkCompletionItem(optArtist->items.front(), "Artist", R"("Artist")", CompletionDetailKind::Frequency, 1, 0);

    auto optListCompletion = completer.complete("$artist in [Ar", 14);
    REQUIRE(optListCompletion);
    CHECK(optListCompletion->replaceBegin == 12);
    CHECK(optListCompletion->replaceEnd == 14);
    CHECK(insertTexts(optListCompletion->items) == std::vector<std::string>{R"("Artist")"});

    auto optConductor = completer.complete("$conductor = Con", 16);
    REQUIRE(optConductor);
    CHECK(optConductor->replaceBegin == 13);
    CHECK(optConductor->replaceEnd == 16);
    CHECK(insertTexts(optConductor->items) == std::vector<std::string>{R"("Conductor")"});

    auto const wordExpression = std::string{"$artist = pinnock"};
    auto optWordPrefix = completer.complete(wordExpression, wordExpression.size());
    REQUIRE(optWordPrefix);
    CHECK(insertTexts(optWordPrefix->items) == std::vector<std::string>{R"("Trevor Pinnock")"});

    CHECK_FALSE(completer.complete(R"(%"Mood" = Br)", 12));
  }

  TEST_CASE("QueryExpressionCompleter - romanized value completion inserts the original expression value",
            "[runtime][unit][completion-query][completion-alias]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "One", .artist = "周杰倫"});
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto aliasPolicyPtr = i18n::createIcuCompletionAliasPolicy();
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(libraryFixture, changesPtr, servicePtr, aliasPolicyPtr.get());
    auto const expression = std::string{"$artist = zhoujielun"};
    auto const optResult = completer.complete(expression, expression.size());

    REQUIRE(optResult);
    CHECK(optResult->replaceBegin == std::string_view{"$artist = "}.size());
    CHECK(optResult->replaceEnd == expression.size());
    REQUIRE(insertTexts(optResult->items) == std::vector<std::string>{R"("周杰倫")"});
    CHECK(optResult->items.front().displayText == "周杰倫");
  }

  TEST_CASE("QueryExpressionCompleter - completes tag and custom-key variables", "[runtime][unit][completion-query]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto tags = std::vector<std::string>{"90s Rock", "Rock"};
    auto custom = std::vector<std::pair<std::string, std::string>>{{"Replay Gain", "-6"}, {"Mood", "Bright"}};
    addCompletionTrack(libraryFixture, tags, custom);

    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(libraryFixture, changesPtr, servicePtr);

    auto optTag = completer.complete("#90", 3);
    REQUIRE(optTag);
    REQUIRE(insertTexts(optTag->items) == std::vector<std::string>{R"(#"90s Rock")"});
    checkCompletionItem(
      optTag->items.front(), R"(#"90s Rock")", R"(#"90s Rock")", CompletionDetailKind::Frequency, 1, 0);

    auto optBareTag = completer.complete("#rock", 5);
    REQUIRE(optBareTag);
    CHECK(optBareTag->replaceBegin == 0);
    CHECK(optBareTag->replaceEnd == 5);
    REQUIRE(insertTexts(optBareTag->items) == std::vector<std::string>{"#Rock", R"(#"90s Rock")"});
    checkCompletionItem(optBareTag->items[0], "#Rock", "#Rock", CompletionDetailKind::Frequency, 1, 0);
    checkCompletionItem(
      optBareTag->items[1], R"(#"90s Rock")", R"(#"90s Rock")", CompletionDetailKind::Frequency, 1, 1);

    auto optCustomKey = completer.complete("%Replay", 7);
    REQUIRE(optCustomKey);
    REQUIRE(insertTexts(optCustomKey->items) == std::vector<std::string>{R"(%"Replay Gain")"});
    checkCompletionItem(
      optCustomKey->items.front(), R"(%"Replay Gain")", R"(%"Replay Gain")", CompletionDetailKind::Frequency, 1, 0);
  }

  TEST_CASE("QueryExpressionCompleter - respects limits and token boundaries",
            "[runtime][unit][completion-query][boundary]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changesPtr = std::unique_ptr<LibraryChanges>{};
    auto servicePtr = std::unique_ptr<CompletionService>{};
    auto completer = makeCompleter(libraryFixture, changesPtr, servicePtr);

    auto optLimited = completer.complete("$", 1, 2);
    REQUIRE(optLimited);
    CHECK(optLimited->items.size() == 2);
    CHECK(insertTexts(optLimited->items) == std::vector<std::string>{"$title", "$artist"});

    CHECK_FALSE(completer.complete(R"("$artist")", 5));
    CHECK_FALSE(completer.complete("$artist", 3));
    CHECK_FALSE(completer.complete("$", 1, 0));
  }
} // namespace ao::rt::test
