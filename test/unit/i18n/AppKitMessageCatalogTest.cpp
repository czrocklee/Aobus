// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/i18n/MessageCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace ao::i18n::test
{
  TEST_CASE("AppKit messages - editor counts use singular and grouped plural forms", "[core][regression][catalog]")
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

  TEST_CASE("AppKit messages - missing shell translation retains English message grammar",
            "[core][regression][catalog]")
  {
    auto catalogRes = MessageCatalog::create("ja-JP");
    REQUIRE(catalogRes);
    auto messageRes = catalogRes->format(MessageId::AppKitDeletingLists, {{"count", 1}});
    REQUIRE(messageRes);
    CHECK(messageRes->text == "1 list will be removed");
    CHECK(messageRes->locale == "en");

    CHECK(requiredFormat(*catalogRes, MessageId::AppKitFieldActionsFor, {{"field", "作品 {title}"}}) ==
          "Actions for 作品 {title}");
  }
} // namespace ao::i18n::test
