// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("LayoutComponents - the registry describes every shared component the way the vocabulary does",
            "[gtk][unit][layout-component][registry]")
  {
    auto fixture = LayoutRuntimeFixture{};

    for (auto const& shared : sharedComponentSchemas())
    {
      CAPTURE(shared.id);
      auto const optRegistered = fixture.components().schema().component(shared.id);
      REQUIRE(optRegistered);
      CHECK(optRegistered->displayName == shared.displayName);
      CHECK(optRegistered->category == shared.category);
      CHECK(optRegistered->minChildren == shared.minChildren);
      CHECK(optRegistered->optMaxChildren == shared.optMaxChildren);
      CHECK(optRegistered->persistentState == shared.persistentState);
      CHECK((optRegistered->actionSlots & shared.actionSlots) == shared.actionSlots);

      for (auto const& sharedProperty : shared.properties)
      {
        auto const registeredProperty =
          std::ranges::find(optRegistered->properties, sharedProperty.name, &uimodel::PropertySchema::name);
        REQUIRE(registeredProperty != optRegistered->properties.end());
        CHECK(registeredProperty->kind == sharedProperty.kind);
        CHECK(registeredProperty->defaultValue.data == sharedProperty.defaultValue.data);
        CHECK(registeredProperty->enumValues == sharedProperty.enumValues);
      }
    }
  }

  TEST_CASE("LayoutComponents - the soul button keeps the secondary hold GDK can tell apart",
            "[gtk][unit][layout-component][registry]")
  {
    // The shared floor stops at the three slots Windows can bind, because a
    // schema that offers a slot its shell refuses fails the document at build
    // time. GDK does raise a distinct secondary hold, so this shell widens the
    // slot back rather than losing a gesture it has always had.
    auto fixture = LayoutRuntimeFixture{};
    auto const optSoul = fixture.components().schema().component("playback.soulButton");

    REQUIRE(optSoul);
    CHECK(optSoul->allows(uimodel::ActionSlot::SecondaryLongPress));
  }

  TEST_CASE("LayoutComponents - standard layout registry creates status and semantic components",
            "[gtk][unit][layout-component][registry]")
  {
    auto fixture = LayoutRuntimeFixture{};

    SECTION("all registered status and semantic types")
    {
      auto const types = std::to_array<std::string_view>({"status.message",
                                                          "library.listTree",
                                                          "track.table",
                                                          "library.openLibraryButton",
                                                          "app.menuBar",
                                                          "status.playbackDetails",
                                                          "status.nowPlaying",
                                                          "status.importProgress",
                                                          "status.notification",
                                                          "status.trackCount",
                                                          "track.detailScope",
                                                          "track.selectionRegion",
                                                          "track.coverArt",
                                                          "track.fieldGrid",
                                                          "track.detailUndoBar",
                                                          "track.tagEditor",
                                                          "track.quickFilter"});

      for (auto const type : types)
      {
        auto const node = LayoutNode{.type = std::string{type}};
        std::unique_ptr<LayoutComponent> const compPtr = fixture.create(node);
        CHECK(compPtr != nullptr);
      }
    }
  }
} // namespace ao::gtk::layout::test
