// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>

namespace ao::gtk::layout::editor::test
{
  using namespace uimodel;

  TEST_CASE("Component descriptor validation covers all standard layout components", "[gtk][unit][layout][editor]")
  {
    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto const& descriptors = registry.descriptors();

    SECTION("all 26 component types have descriptors")
    {
      CHECK(descriptors.size() >= 26);
    }

    SECTION("all descriptors have non-empty type")
    {
      for (auto const& desc : descriptors)
      {
        CHECK(!desc.type.empty());
      }
    }

    SECTION("all descriptors have non-empty displayName")
    {
      for (auto const& desc : descriptors)
      {
        CHECK(!desc.displayName.empty());
      }
    }

    SECTION("all descriptors have a category")
    {
      for (auto const& desc : descriptors)
      {
        CHECK(!uimodel::toString(desc.category).empty());
      }
    }

    SECTION("container types are derived from child limits")
    {
      auto const expectedContainers = std::set<std::string>{"box", "split", "scroll", "tabs"};

      for (auto const& desc : descriptors)
      {
        if (auto const isContainer = uimodel::isContainer(desc); expectedContainers.contains(desc.type))
        {
          CHECK(isContainer);
        }
        else if (!isContainer)
        {
          CHECK(desc.optMaxChildren.value_or(0) == 0);
        }
      }
    }

    SECTION("split requires exactly 2 children")
    {
      auto const optDesc = registry.descriptor("split");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->minChildren == 2);
      REQUIRE(optDesc->optMaxChildren.has_value());
      CHECK(*optDesc->optMaxChildren == 2);
    }

    SECTION("scroll requires exactly 1 child")
    {
      auto const optDesc = registry.descriptor("scroll");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->minChildren == 1);
      REQUIRE(optDesc->optMaxChildren.has_value());
      CHECK(*optDesc->optMaxChildren == 1);
    }

    SECTION("tabs requires at least 1 child")
    {
      auto const optDesc = registry.descriptor("tabs");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->minChildren == 1);
      CHECK(!optDesc->optMaxChildren.has_value()); // unbounded
    }

    SECTION("box has orientation, spacing, homogeneous props")
    {
      auto const optDesc = registry.descriptor("box");

      REQUIRE(optDesc.has_value());
      CHECK(uimodel::isContainer(*optDesc));

      auto const hasProp = [&](std::string const& name)
      { return std::ranges::any_of(optDesc->props, [&](auto const& prop) { return prop.name == name; }); };

      CHECK(hasProp("orientation"));
      CHECK(hasProp("spacing"));
      CHECK(hasProp("homogeneous"));
    }

    SECTION("playPauseButton has showLabel and size props")
    {
      auto const optDesc = registry.descriptor("playback.playPauseButton");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->category == LayoutComponentCategory::Playback);

      auto const hasProp = [&](std::string const& name)
      { return std::ranges::any_of(optDesc->props, [&](auto const& prop) { return prop.name == name; }); };

      CHECK(hasProp("showLabel"));
      CHECK(hasProp("size"));
    }

    SECTION("playback.qualityIndicator has gesture action props")
    {
      auto const optDesc = registry.descriptor("playback.qualityIndicator");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->category == LayoutComponentCategory::Playback);

      auto const hasProp = [&](std::string const& name)
      {
        return std::any_of(optDesc->props.begin(), optDesc->props.end(), [&](auto const& p) { return p.name == name; });
      };

      CHECK_FALSE(hasProp("primaryAction"));
      CHECK_FALSE(hasProp("primaryLongPressAction"));
      CHECK(hasProp("secondaryAction"));
      CHECK(hasProp("secondaryLongPressAction"));
    }

    SECTION("descriptor returns nullopt for unknown type")
    {
      auto const optDesc = registry.descriptor("nonexistent.component");
      CHECK(!optDesc.has_value());
    }

    SECTION("categories span expected groups")
    {
      auto categories = std::set<std::string>{};

      for (auto const& desc : descriptors)
      {
        categories.insert(std::string{uimodel::toString(desc.category)});
      }

      CHECK(categories.contains("Containers"));
      CHECK(categories.contains("Decorators"));
      CHECK(categories.contains("Playback"));
      CHECK(categories.contains("Application"));
      CHECK(categories.contains("Status"));
      CHECK(categories.contains("Library"));
      CHECK(categories.contains("Tracks"));
    }

    SECTION("representative component types are individually retrievable")
    {
      auto const types = std::to_array<std::string_view>({"box",
                                                          "split",
                                                          "scroll",
                                                          "spacer",
                                                          "separator",
                                                          "tabs",
                                                          "playback.playPauseButton",
                                                          "playback.stopButton",
                                                          "playback.volumeControl",
                                                          "playback.currentTitleLabel",
                                                          "playback.currentArtistLabel",
                                                          "playback.seekSlider",
                                                          "playback.timeLabel",
                                                          "playback.playButton",
                                                          "playback.pauseButton",
                                                          "playback.qualityIndicator",
                                                          "playback.qualityIndicator",
                                                          "status.messageLabel",
                                                          "library.listTree",
                                                          "tracks.table",
                                                          "library.openLibraryButton",
                                                          "app.menuBar",
                                                          "track.detailScope",
                                                          "track.selectionRegion",
                                                          "track.coverArt",
                                                          "track.fieldGrid",
                                                          "track.detailUndoBar",
                                                          "track.tagEditor"});

      for (auto const& type : types)
      {
        auto const optDesc = registry.descriptor(std::string{type});
        CHECK(optDesc.has_value());
      }
    }
  }
} // namespace ao::gtk::layout::editor::test
