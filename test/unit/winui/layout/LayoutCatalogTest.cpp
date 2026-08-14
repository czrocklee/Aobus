// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/LayoutCatalog.h>

#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
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
    CHECK_FALSE(catalog.descriptor("collapsibleSplit").has_value());
  }

  TEST_CASE("layoutCatalog - describes every shared component the way the vocabulary does", "[winui][unit][layout]")
  {
    // A Windows-only change cannot quietly give a shared type a second meaning:
    // whatever this catalog registers under a shared name has to match the one
    // definition both shells build from.
    auto const departures = uimodel::sharedVocabularyDepartures(layoutCatalog());

    for (auto const& departure : departures)
    {
      UNSCOPED_INFO(departure);
    }

    CHECK(departures.empty());
  }

  TEST_CASE("layoutCatalog - action slots are injected only where the policy allows them", "[winui][unit][layout]")
  {
    auto const catalog = layoutCatalog();

    // Asserting the primary slot alone is how this shell quietly lost its
    // right-click binding once: adopting a shared descriptor narrowed the
    // policy, the binder still implemented the gesture, and nothing noticed.
    // Every slot `ActionBinder` binds is named here - and the one it refuses is
    // named too, because offering a slot the binder rejects fails the whole
    // node at build time after the document already validated.
    auto const optButton = catalog.descriptor("actionButton");
    REQUIRE(optButton);

    for (auto const slot : {uimodel::LayoutActionSlot::PrimaryClick,
                            uimodel::LayoutActionSlot::PrimaryLongPress,
                            uimodel::LayoutActionSlot::SecondaryClick})
    {
      INFO("slot " << static_cast<int>(slot));
      CHECK(optButton->actionPolicy.isSlotAllowed(slot));
    }

    CHECK_FALSE(optButton->actionPolicy.isSlotAllowed(uimodel::LayoutActionSlot::SecondaryLongPress));

    auto const optLabel = catalog.descriptor("label");
    REQUIRE(optLabel);
    CHECK_FALSE(optLabel->actionPolicy.isSlotAllowed(uimodel::LayoutActionSlot::PrimaryClick));
  }

  TEST_CASE("layoutCatalog - no component offers a slot this shell cannot bind", "[winui][unit][layout]")
  {
    // The catalog decides what a document may author and `ActionBinder` decides
    // what the shell can honor. Where they disagree, a document passes
    // validation and is then rejected in full while being built, which reads to
    // the author as the shell breaking on a layout it just accepted.
    auto const catalog = layoutCatalog();

    for (auto const& descriptor : catalog.descriptors())
    {
      INFO(descriptor.type);
      CHECK_FALSE(descriptor.actionPolicy.isSlotAllowed(uimodel::LayoutActionSlot::SecondaryLongPress));
    }
  }

  TEST_CASE("layoutCatalog - localization is a shell property, not a second meaning for shared text",
            "[winui][unit][layout]")
  {
    // `text` is the words a reader sees, and a document that sets it must read
    // the same in every shell. Resolving it against this shell's resource
    // dictionary made `text: AppTitleValue` show "Aobus" here and
    // "AppTitleValue" in GTK - one property, two meanings. Naming a resource is
    // this shell's own property instead.
    auto const catalog = layoutCatalog();

    for (auto const* const type : {"label", "actionButton", "menuButton"})
    {
      INFO(type);
      auto const optDescriptor = catalog.descriptor(type);
      REQUIRE(optDescriptor);

      auto const named = [&optDescriptor](std::string_view const name)
      { return std::ranges::any_of(optDescriptor->props, [name](auto const& prop) { return prop.name == name; }); };

      CHECK(named(uimodel::kTextProp));
      CHECK(named("textResourceKey"));
    }
  }

  TEST_CASE("layoutCatalog - the soul button names its own inner mark", "[winui][unit][layout]")
  {
    // GTK's `glyph` picks between two static ornaments; this shell draws the
    // live transport icon and only decides whether to draw it. Sharing the name
    // would have one property answering two questions.
    auto const catalog = layoutCatalog();
    auto const optSoul = catalog.descriptor("playback.soulButton");
    REQUIRE(optSoul);

    auto const named = [&optSoul](std::string_view const name)
    { return std::ranges::any_of(optSoul->props, [name](auto const& prop) { return prop.name == name; }); };

    CHECK(named("showGlyph"));
    CHECK_FALSE(named("glyph"));
    // The shared soul properties both shells honor identically stay shared.
    CHECK(named(uimodel::kStrokeWidthProp));
    CHECK(named(uimodel::kGlyphScaleProp));
  }

  TEST_CASE("layoutCatalog - text-bearing components carry the shared text property", "[winui][unit][layout]")
  {
    auto const catalog = layoutCatalog();

    auto const hasProperty = [&catalog](std::string_view const type, std::string_view const property)
    {
      auto const optDescriptor = catalog.descriptor(type);
      REQUIRE(optDescriptor);
      return std::ranges::any_of(optDescriptor->props, [property](auto const& prop) { return prop.name == property; });
    };

    CHECK(hasProperty("label", uimodel::kTextProp));
    CHECK(hasProperty("actionButton", uimodel::kTextProp));
    // A glyph-only menu button still needs a tooltip and an automation name.
    CHECK(hasProperty("menuButton", uimodel::kTextProp));

    // The library root is shell state, not authored text, so it carries none.
    CHECK_FALSE(hasProperty("windows.libraryPath", uimodel::kTextProp));
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
