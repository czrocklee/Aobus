// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/LayoutCatalog.h>

#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/winui/layout/ElementKind.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui::test
{
  using uimodel::CoverArtPlaceholderSlot;
  using uimodel::coverArtPlaceholderStyleId;
  using uimodel::defaultCoverArtPlaceholderStyle;
  using uimodel::LayoutNode;
  using uimodel::LayoutPropertyDescriptor;
  using uimodel::LayoutValue;

  TEST_CASE("layoutCatalog - stays narrower than the GTK catalog", "[winui][unit][layout]")
  {
    auto const catalog = layoutCatalog();

    CHECK(catalog.descriptor("track.table").has_value());
    CHECK(catalog.descriptor("windows.navigationPane").has_value());
    CHECK(catalog.descriptor("windows.inspectorPane").has_value());
    // Shared with the GTK catalog rather than spelled differently for Windows.
    CHECK(catalog.descriptor("track.coverArt").has_value());

    // GTK-only vocabulary the first Windows adoption deliberately omits.
    CHECK_FALSE(catalog.descriptor("tabs").has_value());
    CHECK_FALSE(catalog.descriptor("absoluteCanvas").has_value());
    CHECK_FALSE(catalog.descriptor("responsiveClass").has_value());
    CHECK_FALSE(catalog.descriptor("app.menuBar").has_value());
  }

  TEST_CASE("layoutCatalog - action slots are injected only where the policy allows them", "[winui][unit][layout]")
  {
    auto const catalog = layoutCatalog();

    auto const optButton = catalog.descriptor("actionButton");
    REQUIRE(optButton);
    CHECK(optButton->actionPolicy.isSlotAllowed(uimodel::LayoutActionSlot::PrimaryClick));

    auto const optLabel = catalog.descriptor("label");
    REQUIRE(optLabel);
    CHECK_FALSE(optLabel->actionPolicy.isSlotAllowed(uimodel::LayoutActionSlot::PrimaryClick));
  }

  TEST_CASE("layoutCatalog - text-bearing components name their string through a resource key", "[winui][unit][layout]")
  {
    auto const catalog = layoutCatalog();

    auto const hasProperty = [&catalog](std::string_view const type, std::string_view const property)
    {
      auto const optDescriptor = catalog.descriptor(type);
      REQUIRE(optDescriptor);
      return std::ranges::any_of(optDescriptor->props, [property](auto const& prop) { return prop.name == property; });
    };

    CHECK(hasProperty("label", "resourceKey"));
    CHECK(hasProperty("actionButton", "resourceKey"));
    // A glyph-only menu button still needs a tooltip and an automation name.
    CHECK(hasProperty("menuButton", "resourceKey"));

    // The library root is shell state, not authored text, so it carries no key.
    CHECK_FALSE(hasProperty("windows.libraryPath", "resourceKey"));
  }

  TEST_CASE("layoutCatalog - the Soul button defaults resolve against the Windows action catalog",
            "[winui][unit][layout]")
  {
    auto const components = layoutCatalog();
    auto const actions = layoutActionCatalog();
    auto const optSoul = components.descriptor("playback.soulButton");
    REQUIRE(optSoul);

    for (auto const& [slot, actionId] : optSoul->actionPolicy.defaultActionIds)
    {
      INFO("default action " << actionId);
      CHECK(actions.descriptor(actionId).has_value());
    }
  }

  TEST_CASE("layoutCatalog - the cover art placeholder is authored from the shared style set", "[winui][unit][layout]")
  {
    auto const optCover = layoutCatalog().descriptor("track.coverArt");
    REQUIRE(optCover);

    auto const it =
      std::ranges::find(optCover->props, std::string_view{"placeholderStyle"}, &LayoutPropertyDescriptor::name);
    REQUIRE(it != optCover->props.end());

    // Whatever a placeholder can be drawn as, a Windows preset can ask for: the
    // set is the shared one, so neither shell offers a style the other cannot.
    CHECK(it->enumValues == uimodel::coverArtPlaceholderStyleIds());
    CHECK(it->defaultValue.asString() ==
          coverArtPlaceholderStyleId(defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot::Inspector)));

    // The cover carries no size of its own: a `styleKey` decides how much room
    // it gets, and the component answers that width with a matching height.
    CHECK_FALSE(
      std::ranges::contains(optCover->props, std::string_view{"targetSize"}, &LayoutPropertyDescriptor::name));
  }

  TEST_CASE("componentElementKind - a presentation property selects the constructed element", "[winui][unit][layout]")
  {
    auto const paneWith = [](std::string presentation)
    {
      return LayoutNode{
        .type = "windows.navigationPane", .props = {{"presentation", LayoutValue{std::move(presentation)}}}};
    };

    CHECK(componentElementKind(paneWith("navigationView")) == ElementKind::NavigationView);
    // The tree presentation reports the cell it shares with its resize thumb,
    // not the tree inside it.
    CHECK(componentElementKind(paneWith("tree")) == ElementKind::Grid);
    // The descriptor default applies when the document authors nothing.
    CHECK(componentElementKind(LayoutNode{.type = "windows.navigationPane"}) == ElementKind::NavigationView);

    // The cover reports the Border that rounds and clips it, not the Image.
    CHECK(componentElementKind(LayoutNode{.type = "track.coverArt"}) == ElementKind::Border);

    CHECK(componentElementKind(LayoutNode{.type = "playback.volumeControl"}) == ElementKind::Button);
    CHECK(componentElementKind(LayoutNode{
            .type = "playback.volumeControl", .props = {{"presentation", LayoutValue{std::string{"inline"}}}}}) ==
          ElementKind::Slider);
  }

  TEST_CASE("componentElementKind - every registered component constructs a known element", "[winui][unit][layout]")
  {
    for (auto const& descriptor : layoutCatalog().descriptors())
    {
      INFO("component " << descriptor.type);
      CHECK(componentElementKind(LayoutNode{.type = descriptor.type}).has_value());
    }

    CHECK_FALSE(componentElementKind(LayoutNode{.type = "tabs"}).has_value());
  }

  TEST_CASE("componentElementKind - structural containers allocate slots as grids", "[winui][unit][layout]")
  {
    CHECK(componentElementKind(LayoutNode{.type = "box"}) == ElementKind::Grid);
    CHECK(componentElementKind(LayoutNode{.type = "split"}) == ElementKind::Grid);
  }

  TEST_CASE("presentationChildCount - only the navigation pane fixes its child count", "[winui][unit][layout]")
  {
    CHECK(presentationChildCount(LayoutNode{.type = "windows.navigationPane"}) == 1);
    CHECK(presentationChildCount(LayoutNode{
            .type = "windows.navigationPane", .props = {{"presentation", LayoutValue{std::string{"tree"}}}}}) == 0);
    CHECK_FALSE(presentationChildCount(LayoutNode{.type = "box"}).has_value());
  }

  TEST_CASE("componentRequiresId - components whose state is reconciled must be locatable", "[winui][unit][layout]")
  {
    CHECK(componentRequiresId("track.table"));
    CHECK(componentRequiresId("track.detail"));
    CHECK(componentRequiresId("windows.navigationPane"));
    CHECK(componentRequiresId("windows.inspectorPane"));
    CHECK_FALSE(componentRequiresId("label"));
    CHECK_FALSE(componentRequiresId("box"));
  }
} // namespace ao::winui::test
