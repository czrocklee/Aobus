// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "i18n/MessageIds.h"
#include <ao/i18n/MessageCatalog.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ao::i18n::test
{
  TEST_CASE("AppKit messages - every message resolves in every maintained locale without English fallback",
            "[core][unit][catalog]")
  {
    // Maintained locales are sparse by policy, but the AppKit desktop is
    // translated as one unit. A missing override would fall back silently, and
    // a new message with arguments fails here until it gains a sample.
    struct ArgumentSample final
    {
      MessageId id;
      std::vector<MessageArgument> arguments;
    };

    auto const samples = std::vector<ArgumentSample>{
      {MessageId::AppKitEditTracks, {{"count", 2}}},
      {MessageId::AppKitUpdatingTracks, {{"count", 2}}},
      {MessageId::AppKitDeletingLists, {{"count", 2}}},
      {MessageId::AppKitFieldActionsFor, {{"field", "F"}}},
      {MessageId::AppKitPropertiesSelection, {{"count", 2}}},
      {MessageId::AppKitUnavailablePresentation, {{"id", "I"}}},
      {MessageId::AppKitPlaybackPositionValue, {{"elapsed", "1:23"}, {"duration", "5:00"}}},
      {MessageId::AppKitSelectionTotal, {{"selected", 1}, {"total", 2}}},
      {MessageId::AppKitTracksSelected, {{"count", 2}}},
      {MessageId::AppKitTrackOfTotal, {{"track", 1}, {"total", 2}}},
      {MessageId::AppKitRecentUnavailable, {{"path", "/P"}}},
      {MessageId::AppKitActivityDetailLabel, {{"severity", "S"}, {"message", "M"}}},
    };

    auto const appKitMessages = std::ranges::count_if(detail::kMessageDefinitions,
                                                      [](detail::MessageDefinition const& definition)
                                                      { return definition.key.starts_with("appkit_"); });
    REQUIRE(appKitMessages > 0);

    for (auto const& locale : availableCatalogLocales())
    {
      if (locale.tag == "en")
      {
        continue;
      }

      INFO(locale.tag);
      auto catalogRes = MessageCatalog::create(locale.tag);
      REQUIRE(catalogRes);

      for (auto const& definition : detail::kMessageDefinitions)
      {
        if (!definition.key.starts_with("appkit_"))
        {
          continue;
        }

        CAPTURE(definition.key);
        auto const sample = std::ranges::find(samples, definition.id, &ArgumentSample::id);
        auto const arguments = sample != samples.end() ? std::span<MessageArgument const>{sample->arguments}
                                                       : std::span<MessageArgument const>{};
        auto const res = catalogRes->format(definition.id, arguments);
        REQUIRE(res);
        CHECK(res->locale == locale.tag);
      }
    }
  }

  TEST_CASE("AppKit messages - editor counts use singular and grouped plural forms", "[core][unit][catalog]")
  {
    auto catalogRes = MessageCatalog::create("en");
    REQUIRE(catalogRes);

    struct Example final
    {
      MessageId message;
      std::uint64_t count;
      std::string_view expected;
    };

    auto const examples = std::array{
      Example{MessageId::AppKitEditTracks, 1, "Edit 1 Track"},
      Example{MessageId::AppKitEditTracks, 50000, "Edit 50,000 Tracks"},
      Example{MessageId::AppKitUpdatingTracks, 1, "Updating 1 selected track"},
      Example{MessageId::AppKitUpdatingTracks, 2, "Updating 2 selected tracks"},
      Example{MessageId::AppKitDeletingLists, 1, "1 list will be removed"},
      Example{MessageId::AppKitDeletingLists, 2, "2 lists will be removed"},
      Example{MessageId::AppKitPropertiesSelection, 1, "Properties — 1 selected track"},
      Example{MessageId::AppKitPropertiesSelection, 0, "Properties — 0 selected tracks"},
      Example{MessageId::AppKitTracksSelected, 1, "1 track selected"},
      Example{MessageId::AppKitTracksSelected, 50000, "50,000 tracks selected"},
    };

    for (auto const& example : examples)
    {
      CHECK(requiredFormat(*catalogRes, example.message, {{"count", example.count}}) == example.expected);
    }
  }

  TEST_CASE("AppKit messages - selection status preserves selected and total counts", "[core][unit][catalog]")
  {
    auto catalogRes = MessageCatalog::create("en");
    REQUIRE(catalogRes);
    CHECK(requiredFormat(*catalogRes, MessageId::AppKitSelectionTotal, {{"selected", 1}, {"total", 1}}) ==
          "1 selected / 1 track");
    CHECK(requiredFormat(*catalogRes, MessageId::AppKitSelectionTotal, {{"selected", 1000}, {"total", 50000}}) ==
          "1,000 selected / 50,000 tracks");
    CHECK(requiredFormat(*catalogRes,
                         MessageId::AppKitPlaybackPositionValue,
                         {{"elapsed", "1:23"}, {"duration", "5:00"}}) == "1:23 of 5:00");
  }

  TEST_CASE("AppKit messages - repeat modes expose distinct accessibility descriptions", "[core][unit][catalog]")
  {
    auto catalogRes = MessageCatalog::create("en");
    REQUIRE(catalogRes);
    CHECK(requiredText(*catalogRes, MessageId::AppKitRepeatOff) == "Repeat Off");
    CHECK(requiredText(*catalogRes, MessageId::AppKitRepeatAll) == "Repeat All");
    CHECK(requiredText(*catalogRes, MessageId::AppKitRepeatOne) == "Repeat One");
  }

  TEST_CASE("AppKit messages - an unsupported locale retains English message grammar", "[core][unit][catalog]")
  {
    // Korean selects the other plural form for one, so the English singular
    // proves that fallback text keeps English grammar.
    auto catalogRes = MessageCatalog::create("ko-KR");
    REQUIRE(catalogRes);
    auto messageRes = catalogRes->format(MessageId::AppKitDeletingLists, {{"count", 1}});
    REQUIRE(messageRes);
    CHECK(messageRes->text == "1 list will be removed");
    CHECK(messageRes->locale == "en");

    CHECK(requiredFormat(*catalogRes, MessageId::AppKitFieldActionsFor, {{"field", "作品 {title}"}}) ==
          "Actions for 作品 {title}");
  }

  TEST_CASE("AppKit messages - Japanese uses its own plural form and keeps argument text", "[core][unit][catalog]")
  {
    auto catalogRes = MessageCatalog::create("ja-JP");
    REQUIRE(catalogRes);
    auto messageRes = catalogRes->format(MessageId::AppKitDeletingLists, {{"count", 1}});
    REQUIRE(messageRes);
    CHECK(messageRes->text == "1 件のリストが削除されます");
    CHECK(messageRes->locale == "ja");

    CHECK(requiredFormat(*catalogRes, MessageId::AppKitFieldActionsFor, {{"field", "作品 {title}"}}) ==
          "作品 {title} の操作");
  }
} // namespace ao::i18n::test
