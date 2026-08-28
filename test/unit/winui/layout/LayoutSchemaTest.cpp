// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/LayoutSchema.h>

#include <ao/uimodel/layout/component/LayoutSchema.h>
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
  using uimodel::LayoutValue;
  using uimodel::PropertySchema;

  TEST_CASE("layoutSchema - every shared id preserves the canonical vocabulary floor", "[winui][unit][layout]")
  {
    auto const schema = layoutSchema();

    for (auto const& shared : uimodel::sharedComponentSchemas())
    {
      CAPTURE(shared.id);
      auto const optRegistered = schema.component(shared.id);
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
          std::ranges::find(optRegistered->properties, sharedProperty.name, &PropertySchema::name);
        REQUIRE(registeredProperty != optRegistered->properties.end());
        CHECK(registeredProperty->kind == sharedProperty.kind);
        CHECK(registeredProperty->defaultValue.data == sharedProperty.defaultValue.data);
        CHECK(registeredProperty->enumValues == sharedProperty.enumValues);
      }
    }
  }

  TEST_CASE("layoutSchema - stays narrower than the GTK schema", "[winui][unit][layout]")
  {
    auto const schema = layoutSchema();

    CHECK(schema.component("track.table").has_value());
    CHECK(schema.component("windows.navigationPane").has_value());
    CHECK(schema.component("windows.inspectorPane").has_value());
    // Shared with the GTK schema rather than spelled differently for Windows.
    CHECK(schema.component("track.coverArt").has_value());

    // GTK-only vocabulary the first Windows adoption deliberately omits.
    CHECK_FALSE(schema.component("tabs").has_value());
    CHECK_FALSE(schema.component("absoluteCanvas").has_value());
    CHECK_FALSE(schema.component("responsiveClass").has_value());
    CHECK_FALSE(schema.component("collapsibleSplit").has_value());
  }

  TEST_CASE("layoutSchema - action slots are injected only where the policy allows them", "[winui][unit][layout]")
  {
    auto const schema = layoutSchema();

    // Asserting the primary slot alone is how this shell quietly lost its
    // right-click binding once: adopting a shared component narrowed the
    // policy, the binder still implemented the gesture, and nothing noticed.
    // Every slot `ActionBinder` binds is named here - and the one it refuses is
    // named too, because offering a slot the binder rejects fails the whole
    // node at build time after the document already validated.
    auto const optButton = schema.component("actionButton");
    REQUIRE(optButton);

    for (auto const slot : {uimodel::ActionSlot::PrimaryClick,
                            uimodel::ActionSlot::PrimaryLongPress,
                            uimodel::ActionSlot::SecondaryClick})
    {
      INFO("slot " << static_cast<int>(slot));
      CHECK(optButton->allows(slot));
    }

    CHECK_FALSE(optButton->allows(uimodel::ActionSlot::SecondaryLongPress));

    auto const optLabel = schema.component("label");
    REQUIRE(optLabel);
    CHECK_FALSE(optLabel->allows(uimodel::ActionSlot::PrimaryClick));
  }

  TEST_CASE("layoutSchema - no component offers a slot this shell cannot bind", "[winui][unit][layout]")
  {
    // The schema decides what a document may author and `ActionBinder` decides
    // what the shell can honor. Where they disagree, a document passes
    // validation and is then rejected in full while being built, which reads to
    // the author as the shell breaking on a layout it just accepted.
    auto const schema = layoutSchema();

    for (auto const& component : schema.components())
    {
      INFO(component.id);
      CHECK_FALSE(component.allows(uimodel::ActionSlot::SecondaryLongPress));
    }
  }

  TEST_CASE("layoutSchema - localization is a shell property, not a second meaning for shared text",
            "[winui][unit][layout]")
  {
    // `text` is the words a reader sees, and a document that sets it must read
    // the same in every shell. Resolving it against this shell's resource
    // dictionary made `text: AppTitleValue` show "Aobus" here and
    // "AppTitleValue" in GTK - one property, two meanings. Naming a resource is
    // this shell's own property instead.
    auto const schema = layoutSchema();

    for (auto const* const type : {"label", "actionButton", "menuButton"})
    {
      INFO(type);
      auto const optComponentSchema = schema.component(type);
      REQUIRE(optComponentSchema);

      auto const named = [&optComponentSchema](std::string_view const name)
      {
        return std::ranges::any_of(
          optComponentSchema->properties, [name](auto const& prop) { return prop.name == name; });
      };

      CHECK(named(uimodel::kTextProp));
      CHECK(named("textResourceKey"));
    }
  }

  TEST_CASE("layoutSchema - the soul button names its own inner mark", "[winui][unit][layout]")
  {
    // GTK's `glyph` picks between two static ornaments; this shell draws the
    // live transport icon and only decides whether to draw it. Sharing the name
    // would have one property answering two questions.
    auto const schema = layoutSchema();
    auto const optSoul = schema.component("playback.soulButton");
    REQUIRE(optSoul);

    auto const named = [&optSoul](std::string_view const name)
    { return std::ranges::any_of(optSoul->properties, [name](auto const& prop) { return prop.name == name; }); };

    CHECK(named("showGlyph"));
    CHECK_FALSE(named("glyph"));
    // The shared soul properties both shells honor identically stay shared.
    CHECK(named(uimodel::kStrokeWidthProp));
    CHECK(named(uimodel::kGlyphScaleProp));
  }

  TEST_CASE("layoutSchema - text-bearing components carry the shared text property", "[winui][unit][layout]")
  {
    auto const schema = layoutSchema();

    auto const hasProperty = [&schema](std::string_view const type, std::string_view const property)
    {
      auto const optComponentSchema = schema.component(type);
      REQUIRE(optComponentSchema);
      return std::ranges::any_of(
        optComponentSchema->properties, [property](auto const& prop) { return prop.name == property; });
    };

    CHECK(hasProperty("label", uimodel::kTextProp));
    CHECK(hasProperty("actionButton", uimodel::kTextProp));
    // A glyph-only menu button still needs a tooltip and an automation name.
    CHECK(hasProperty("menuButton", uimodel::kTextProp));

    // The library root is shell state, not authored text, so it carries none.
    CHECK_FALSE(hasProperty("windows.libraryPath", uimodel::kTextProp));
  }

  TEST_CASE("layoutSchema - the Soul button defaults resolve against the Windows action schema",
            "[winui][unit][layout]")
  {
    auto const schema = layoutSchema();
    auto const optSoul = schema.component("playback.soulButton");
    REQUIRE(optSoul);

    for (auto const& [slot, actionId] : optSoul->defaultActions)
    {
      INFO("default action " << actionId);
      CHECK(schema.action(actionId).has_value());
    }
  }

  TEST_CASE("layoutSchema - the cover art placeholder is authored from the shared style set", "[winui][unit][layout]")
  {
    auto const optCover = layoutSchema().component("track.coverArt");
    REQUIRE(optCover);

    auto const it =
      std::ranges::find(optCover->properties, std::string_view{"placeholderStyle"}, &PropertySchema::name);
    REQUIRE(it != optCover->properties.end());

    // Whatever a placeholder can be drawn as, a Windows preset can ask for: the
    // set is the shared one, so neither shell offers a style the other cannot.
    CHECK(it->enumValues == uimodel::coverArtPlaceholderStyleIds());
    CHECK(it->defaultValue.asString() ==
          coverArtPlaceholderStyleId(defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot::Inspector)));

    // The cover carries no size of its own: a `styleKey` decides how much room
    // it gets, and the component answers that width with a matching height.
    CHECK_FALSE(std::ranges::contains(optCover->properties, std::string_view{"targetSize"}, &PropertySchema::name));
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
    // The component default applies when the document authors nothing.
    CHECK(componentElementKind(LayoutNode{.type = "windows.navigationPane"}) == ElementKind::NavigationView);

    // The cover reports the Border that rounds and clips it, not the Image.
    CHECK(componentElementKind(LayoutNode{.type = "track.coverArt"}) == ElementKind::Border);

    CHECK(componentElementKind(LayoutNode{.type = "track.quickFilter"}) == ElementKind::Grid);

    CHECK(componentElementKind(LayoutNode{.type = "playback.volumeControl"}) == ElementKind::Button);
    CHECK(componentElementKind(LayoutNode{
            .type = "playback.volumeControl", .props = {{"presentation", LayoutValue{std::string{"inline"}}}}}) ==
          ElementKind::Slider);
  }

  TEST_CASE("componentElementKind - every registered component constructs a known element", "[winui][unit][layout]")
  {
    for (auto const& component : layoutSchema().components())
    {
      INFO("component " << component.id);
      CHECK(componentElementKind(LayoutNode{.type = component.id}).has_value());
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
