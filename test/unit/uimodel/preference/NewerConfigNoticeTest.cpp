// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/preference/NewerConfigNotice.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>

namespace ao::uimodel::test
{
  TEST_CASE("NewerConfigNotice - each member keeps one retained warning", "[uimodel][unit][preference]")
  {
    auto executor = rt::test::InlineExecutor{};
    auto asyncRuntime = async::Runtime{executor, 1};
    auto notifications = rt::NotificationService{asyncRuntime};
    auto const& catalog = ao::test::englishMessageCatalog();

    reportNewerConfig(notifications, catalog, "shortcuts", i18n::MessageId::ConfigNewerShortcuts);
    reportNewerConfig(notifications, catalog, "shortcuts", i18n::MessageId::ConfigNewerShortcuts);

    auto const feed = notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    auto const& entry = feed.entries.front();
    CHECK(entry.severity == rt::NotificationSeverity::Warning);
    CHECK(entry.lifetime == rt::NotificationLifetime::history());
    REQUIRE(std::holds_alternative<std::string>(entry.message));
    CHECK(std::get<std::string>(entry.message) == i18n::requiredText(catalog, i18n::MessageId::ConfigNewerShortcuts));

    // A different notice for the same member takes the existing entry's place.
    reportNewerConfig(notifications, catalog, "shortcuts", i18n::MessageId::TuiConfigNewerSettings);

    auto const replaced = notifications.feed();
    REQUIRE(replaced.entries.size() == 1);
    CHECK(replaced.entries.front().id == entry.id);
    REQUIRE(std::holds_alternative<std::string>(replaced.entries.front().message));
    CHECK(std::get<std::string>(replaced.entries.front().message) ==
          i18n::requiredText(catalog, i18n::MessageId::TuiConfigNewerSettings));

    reportNewerConfig(notifications, catalog, "layout", i18n::MessageId::GtkConfigNewerLayout);

    CHECK(notifications.feed().entries.size() == 2);
  }
} // namespace ao::uimodel::test
