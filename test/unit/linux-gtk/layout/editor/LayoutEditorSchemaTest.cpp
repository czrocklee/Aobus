// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/ShellLayoutCollaborators.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout::editor::test
{
  using namespace uimodel;

  TEST_CASE("LayoutEditorSchema - schema entry validation covers all standard layout components",
            "[gtk][unit][layout][editor]")
  {
    auto const tempDir = ao::test::TempDir{};
    std::unique_ptr<rt::AppRuntime> runtimePtr = ao::gtk::test::makeRuntime(tempDir);
    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(
      registry, *runtimePtr, ShellLayoutCollaborators{.textCatalog = ao::test::englishMessageCatalog()});

    auto const& schemas = registry.schema().components();

    SECTION("all 26 component types have schema entries")
    {
      CHECK(schemas.size() >= 26);
    }

    SECTION("all schema entries have non-empty type")
    {
      for (auto const& schema : schemas)
      {
        CHECK(!schema.id.empty());
      }
    }

    SECTION("all schema entries have non-empty displayName")
    {
      for (auto const& schema : schemas)
      {
        CHECK(!schema.displayName.empty());
      }
    }

    SECTION("all schema entries have a category")
    {
      for (auto const& schema : schemas)
      {
        CHECK(!uimodel::toString(schema.category).empty());
      }
    }

    SECTION("container classification exactly matches the standard component inventory")
    {
      auto const expectedContainers = std::set<std::string>{"absoluteCanvas",
                                                            "box",
                                                            "centerBox",
                                                            "collapsibleSplit",
                                                            "responsiveClass",
                                                            "scroll",
                                                            "split",
                                                            "tabs",
                                                            "track.detailScope",
                                                            "track.selectionRegion",
                                                            "workspace.withDetailPane"};

      for (auto const& schema : schemas)
      {
        CAPTURE(schema.id);
        CHECK(uimodel::isContainer(schema) == expectedContainers.contains(schema.id));

        if (!expectedContainers.contains(schema.id))
        {
          REQUIRE(schema.optMaxChildren);
          CHECK(*schema.optMaxChildren == 0);
        }
      }
    }

    SECTION("split requires exactly 2 children")
    {
      auto const optComponentSchema = registry.schema().component("split");

      REQUIRE(optComponentSchema);
      CHECK(optComponentSchema->minChildren == 2);
      REQUIRE(optComponentSchema->optMaxChildren);
      CHECK(*optComponentSchema->optMaxChildren == 2);
    }

    SECTION("scroll requires exactly 1 child")
    {
      auto const optComponentSchema = registry.schema().component("scroll");

      REQUIRE(optComponentSchema);
      CHECK(optComponentSchema->minChildren == 1);
      REQUIRE(optComponentSchema->optMaxChildren);
      CHECK(*optComponentSchema->optMaxChildren == 1);
    }

    SECTION("tabs requires at least 1 child")
    {
      auto const optComponentSchema = registry.schema().component("tabs");

      REQUIRE(optComponentSchema);
      CHECK(optComponentSchema->minChildren == 1);
      CHECK(!optComponentSchema->optMaxChildren); // unbounded
    }

    SECTION("box has orientation, spacing, homogeneous props")
    {
      auto const optComponentSchema = registry.schema().component("box");

      REQUIRE(optComponentSchema);
      CHECK(uimodel::isContainer(*optComponentSchema));

      auto const hasProp = [&](std::string const& name)
      {
        return std::ranges::any_of(optComponentSchema->properties, [&](auto const& prop) { return prop.name == name; });
      };

      CHECK(hasProp("orientation"));
      CHECK(hasProp("spacing"));
      CHECK(hasProp("homogeneous"));
    }

    SECTION("transportButton has command, showLabel, and size props")
    {
      auto const optComponentSchema = registry.schema().component("playback.transportButton");

      REQUIRE(optComponentSchema);
      CHECK(optComponentSchema->category == ComponentCategory::Playback);

      auto const hasProp = [&](std::string const& name)
      {
        return std::ranges::any_of(optComponentSchema->properties, [&](auto const& prop) { return prop.name == name; });
      };

      CHECK(hasProp("showLabel"));
      CHECK(hasProp("size"));
    }

    SECTION("Soul components expose presentation geometry props")
    {
      for (auto const* const type : {"playback.soulButton", "playback.soulPlayPauseButton"})
      {
        auto const optComponentSchema = registry.schema().component(type);

        REQUIRE(optComponentSchema);
        auto const hasProp = [&](std::string_view const name)
        {
          return std::ranges::any_of(
            optComponentSchema->properties, [name](PropertySchema const& prop) { return prop.name == name; });
        };

        CHECK(hasProp("strokeWidth"));
        CHECK(hasProp("glyphScale"));
      }
    }

    SECTION("playback.qualityIndicator has gesture action props")
    {
      auto const optComponentSchema = registry.schema().component("playback.qualityIndicator");

      REQUIRE(optComponentSchema);
      CHECK(optComponentSchema->category == ComponentCategory::Playback);

      auto const hasProp = [&](std::string const& name)
      {
        return std::ranges::any_of(
          optComponentSchema->properties, [&](auto const& property) { return property.name == name; });
      };

      CHECK_FALSE(hasProp("primaryAction"));
      CHECK_FALSE(hasProp("primaryLongPressAction"));
      CHECK(hasProp("secondaryAction"));
      CHECK(hasProp("secondaryLongPressAction"));
    }

    SECTION("schema entry returns nullopt for unknown type")
    {
      auto const optComponentSchema = registry.schema().component("nonexistent.component");
      CHECK(!optComponentSchema);
    }

    SECTION("categories span expected groups")
    {
      auto categories = std::set<std::string>{};

      for (auto const& schema : schemas)
      {
        categories.insert(std::string{uimodel::toString(schema.category)});
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
                                                          "playback.transportButton",
                                                          "playback.volumeControl",
                                                          "playback.currentTitleLabel",
                                                          "playback.currentArtistLabel",
                                                          "playback.seekSlider",
                                                          "playback.timeLabel",
                                                          "playback.qualityIndicator",
                                                          "playback.qualityIndicator",
                                                          "status.message",
                                                          "library.listTree",
                                                          "track.table",
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
        auto const optComponentSchema = registry.schema().component(std::string{type});
        CHECK(optComponentSchema);
      }
    }

    SECTION("cover-art placeholder choices are exposed as enum properties")
    {
      auto const cases = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"track.table", "groupCoverPlaceholderStyle"},
        {"track.coverArt", "placeholderStyle"},
        {"playback.image", "placeholderStyle"},
      });
      auto const expected = coverArtPlaceholderStyleIds();

      for (auto const& [type, property] : cases)
      {
        auto const optComponentSchema = registry.schema().component(std::string{type});
        REQUIRE(optComponentSchema);
        auto const found = std::ranges::find_if(
          optComponentSchema->properties, [property](auto const& candidate) { return candidate.name == property; });
        REQUIRE(found != optComponentSchema->properties.end());
        CHECK(found->kind == PropertyKind::Enum);
        CHECK(found->enumValues == expected);
      }
    }
  }
} // namespace ao::gtk::layout::editor::test
