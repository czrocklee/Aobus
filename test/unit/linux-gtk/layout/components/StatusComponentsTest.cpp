// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/ComponentRegistrations.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>

namespace ao::gtk::layout::test
{
  TEST_CASE("StatusComponents - status bar components register status schema entries", "[gtk][unit][layout][status]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.status_test");
    auto const tempDir = ao::test::TempDir{};
    std::unique_ptr<rt::AppRuntime> runtimePtr = ao::gtk::test::makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    registerStatusComponents(registry, *runtimePtr, ao::test::englishMessageCatalog());

    struct ExpectedStatusSchema final
    {
      std::string_view id;
      std::string_view displayName;
      bool shared;
    };

    auto constexpr kExpectedSchemas = std::to_array<ExpectedStatusSchema>({
      {.id = "status.activity", .displayName = "Activity Status", .shared = true},
      {.id = "status.message", .displayName = "Status Message", .shared = true},
      {.id = "status.nowPlaying", .displayName = "Now Playing Status", .shared = false},
      {.id = "status.playbackDetails", .displayName = "Playback Details", .shared = false},
      {.id = "status.selectionInfo", .displayName = "Selection Info", .shared = true},
      {.id = "status.trackCount", .displayName = "Track Count", .shared = true},
    });

    CHECK(registry.schema().components().size() == kExpectedSchemas.size());
    auto const sharedSchemas = uimodel::sharedComponentSchemas();

    for (auto const& expected : kExpectedSchemas)
    {
      CAPTURE(expected.id);
      auto const optSchema = registry.schema().component(expected.id);
      REQUIRE(optSchema);
      CHECK(optSchema->displayName == expected.displayName);
      CHECK(optSchema->category == uimodel::ComponentCategory::Status);
      CHECK(optSchema->minChildren == 0);
      REQUIRE(optSchema->optMaxChildren);
      CHECK(*optSchema->optMaxChildren == 0);

      auto const sharedIt = std::ranges::find(sharedSchemas, expected.id, &uimodel::ComponentSchema::id);
      CHECK((sharedIt != sharedSchemas.end()) == expected.shared);

      if (expected.shared)
      {
        REQUIRE(sharedIt != sharedSchemas.end());
        CHECK(optSchema->displayName == sharedIt->displayName);
        CHECK(optSchema->category == sharedIt->category);
        CHECK(optSchema->minChildren == sharedIt->minChildren);
        CHECK(optSchema->optMaxChildren == sharedIt->optMaxChildren);
        CHECK(optSchema->persistentState == sharedIt->persistentState);
        CHECK((optSchema->actionSlots & sharedIt->actionSlots) == sharedIt->actionSlots);
      }
    }

    CHECK_FALSE(registry.schema().component("status.statusSlot").has_value());
    CHECK_FALSE(registry.schema().component("status.notificationCenter").has_value());
  }
} // namespace ao::gtk::layout::test
