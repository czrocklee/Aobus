// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>

#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    LayoutComponentCatalog catalogOf(LayoutComponentDescriptor descriptor)
    {
      auto catalog = LayoutComponentCatalog{};
      catalog.registerComponentDescriptor(std::move(descriptor));
      return catalog;
    }
  } // namespace

  TEST_CASE("SharedLayoutComponentType - every component names one distinct type", "[uimodel][unit][layout]")
  {
    auto seen = std::vector<std::string_view>{};

    for (auto const component : sharedLayoutComponentTypes())
    {
      auto const type = componentTypeName(component);
      INFO("type: " << type);
      CHECK_FALSE(type.empty());
      CHECK(std::ranges::find(seen, type) == seen.end());
      seen.push_back(type);
    }

    CHECK(seen.size() == sharedLayoutComponentTypes().size());
  }

  TEST_CASE("SharedLayoutComponentType - a type name resolves back to its component", "[uimodel][unit][layout]")
  {
    for (auto const component : sharedLayoutComponentTypes())
    {
      auto const optResolved = sharedComponentFor(componentTypeName(component));
      REQUIRE(optResolved);
      CHECK(*optResolved == component);
    }

    CHECK_FALSE(sharedComponentFor("windows.titleBar"));
    CHECK_FALSE(sharedComponentFor(""));
  }

  TEST_CASE("SharedLayoutComponentType - a shared descriptor carries its own type name", "[uimodel][unit][layout]")
  {
    for (auto const component : sharedLayoutComponentTypes())
    {
      auto const descriptor = sharedComponentDescriptor(component);
      INFO("type: " << descriptor.type);
      CHECK(descriptor.type == componentTypeName(component));
      CHECK_FALSE(descriptor.displayName.empty());
      CHECK(descriptor.persistentState == (component == SharedLayoutComponentType::Split));
    }
  }

  TEST_CASE("SharedLayoutComponentType - a shell property extends the shared descriptor", "[uimodel][unit][layout]")
  {
    auto const descriptor =
      withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::Box),
                          {{.name = "homogeneous", .kind = LayoutPropertyKind::Bool, .label = "Homogeneous"}});

    // The shared properties keep their place, and the shell's own follows them.
    REQUIRE(descriptor.props.size() == sharedComponentDescriptor(SharedLayoutComponentType::Box).props.size() + 1);
    CHECK(descriptor.props.back().name == "homogeneous");
    CHECK(descriptor.props.front().name == kOrientationProp);
  }

  TEST_CASE("SharedLayoutComponentType - a faithful catalog departs from nothing", "[uimodel][unit][layout]")
  {
    auto catalog = LayoutComponentCatalog{};

    for (auto const component : sharedLayoutComponentTypes())
    {
      catalog.registerComponentDescriptor(sharedComponentDescriptor(component));
    }

    // A shell's own component is not the vocabulary's business.
    catalog.registerComponentDescriptor({.type = "windows.titleBar", .displayName = "Title Bar"});

    CHECK(sharedVocabularyDepartures(catalog).empty());
  }

  TEST_CASE("SharedLayoutComponentType - a redescribed shared component is reported", "[uimodel][unit][layout]")
  {
    SECTION("a renamed shared property")
    {
      auto descriptor = sharedComponentDescriptor(SharedLayoutComponentType::PlaybackTimeLabel);
      descriptor.props.front().name = "variant";

      auto const departures = sharedVocabularyDepartures(catalogOf(std::move(descriptor)));

      REQUIRE(departures.size() == 1);
      CHECK(departures.front().contains("mode"));
    }

    SECTION("a redefined shared property")
    {
      auto descriptor = sharedComponentDescriptor(SharedLayoutComponentType::TrackPresentationButton);
      descriptor.props.front().enumValues = {"title", "compact"};

      auto const departures = sharedVocabularyDepartures(catalogOf(std::move(descriptor)));

      REQUIRE(departures.size() == 1);
      CHECK(departures.front().contains("variant"));
    }

    SECTION("a different child range")
    {
      auto descriptor = sharedComponentDescriptor(SharedLayoutComponentType::StatusMessage);
      descriptor.optMaxChildren = {};

      auto const departures = sharedVocabularyDepartures(catalogOf(std::move(descriptor)));

      REQUIRE(departures.size() == 1);
      CHECK(departures.front().contains("child range"));
    }

    SECTION("a different action policy")
    {
      auto descriptor = sharedComponentDescriptor(SharedLayoutComponentType::ActionButton);
      descriptor.actionPolicy = kNoExternalActions;

      auto const departures = sharedVocabularyDepartures(catalogOf(std::move(descriptor)));

      REQUIRE(departures.size() == 1);
      CHECK(departures.front().contains("action slots"));
    }

    SECTION("a different display name")
    {
      auto descriptor = sharedComponentDescriptor(SharedLayoutComponentType::TrackTable);
      descriptor.displayName = "Tracks Table";

      auto const departures = sharedVocabularyDepartures(catalogOf(std::move(descriptor)));

      REQUIRE(departures.size() == 1);
      CHECK(departures.front().contains("display name"));
    }

    SECTION("a different persistent-state policy")
    {
      auto descriptor = sharedComponentDescriptor(SharedLayoutComponentType::Split);
      descriptor.persistentState = false;

      auto const departures = sharedVocabularyDepartures(catalogOf(std::move(descriptor)));

      REQUIRE(departures.size() == 1);
      CHECK(departures.front().contains("persistent state"));
    }
  }

  TEST_CASE("SharedLayoutComponentType - a shell may widen the shared action slots but not narrow them",
            "[uimodel][unit][layout]")
  {
    // The shared set is a floor. Comparing it for equality instead is how a
    // shell silently loses a gesture its toolkit already binds: adopting the
    // shared descriptor takes the slot away, and a test that compares against
    // that same descriptor cannot see it happen.
    SECTION("a widened policy is not a departure")
    {
      auto const descriptor =
        withShellActionSlots(sharedComponentDescriptor(SharedLayoutComponentType::ActionButton), kAllExternalActions);

      CHECK(descriptor.actionPolicy.slotMask == kAllExternalActions.slotMask);
      CHECK(sharedVocabularyDepartures(catalogOf(descriptor)).empty());
    }

    SECTION("a shell default action rides along with the slot")
    {
      auto policy = kAllExternalActions;
      policy.defaultActionIds = {{LayoutActionSlot::SecondaryClick, "shell.showSystemMenu"}};

      auto const descriptor =
        withShellActionSlots(sharedComponentDescriptor(SharedLayoutComponentType::ActionButton), policy);

      REQUIRE(descriptor.actionPolicy.defaultActionIds.size() == 1);
      CHECK(descriptor.actionPolicy.defaultActionIds.front().second == "shell.showSystemMenu");
      CHECK(sharedVocabularyDepartures(catalogOf(descriptor)).empty());
    }

    SECTION("dropping a shared slot is still a departure")
    {
      auto descriptor = sharedComponentDescriptor(SharedLayoutComponentType::PlaybackSoulButton);
      descriptor.actionPolicy = kExternalPrimaryActions;

      auto const departures = sharedVocabularyDepartures(catalogOf(std::move(descriptor)));

      REQUIRE(departures.size() == 1);
      CHECK(departures.front().contains("action slots"));
    }
  }

  TEST_CASE("SharedLayoutComponentType - a shell property is not a departure", "[uimodel][unit][layout]")
  {
    auto const descriptor =
      withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::PlaybackVolumeControl),
                          {{.name = "presentation", .kind = LayoutPropertyKind::Enum, .label = "Presentation"}});

    CHECK(sharedVocabularyDepartures(catalogOf(descriptor)).empty());
  }
} // namespace ao::uimodel::test
