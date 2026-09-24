// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/document/LayoutPresets.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutBuildContext.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>
#include <glib-object.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/gesturelongpress.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::findController;

  namespace
  {
    std::vector<Gtk::Widget*> directChildren(Gtk::Widget& parent)
    {
      auto children = std::vector<Gtk::Widget*>{};

      for (auto* child = parent.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        children.push_back(child);
      }

      return children;
    }
  } // namespace

  TEST_CASE("LayoutComponentYaml - YAML action button renders text and dispatches authored actions",
            "[gtk][unit][layout-component][yaml]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& registry = fixture.components();
    auto actionRegistry = ActionRegistry{registry.schema()};
    auto buildSnapshot = activateBuildSnapshot(fixture.session());
    auto actionCtx = LayoutBuildContext{.registry = registry,
                                        .actionRegistry = actionRegistry,
                                        .parentWindow = fixture.window(),
                                        .session = fixture.session(),
                                        .buildSnapshot = std::move(buildSnapshot)};
    auto const* const yaml = R"(
      type: actionButton
      props:
        text: "Settings"
        icon: "emblem-system-symbolic"
        style: "circular"
        primaryAction: "shell.showSystemMenu"
        primaryLongPressAction: "shell.showSoul"
      )";
    auto tree = ryml::Tree{yaml::callbacks()};
    ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
    auto decodedNodeRes = readLayoutNode(tree.rootref(), "action button fixture");
    REQUIRE(decodedNodeRes);
    auto layoutNode = std::move(*decodedNodeRes);

    std::int32_t primaryFired = 0;
    std::int32_t longPressFired = 0;

    actionRegistry.tryRegisterAction(
      ActionSchema{.id = "shell.showSystemMenu", .label = "System Menu", .category = "Shell", .capabilities = 0},
      [&](ActionActivationContext&) { primaryFired++; });

    actionRegistry.tryRegisterAction(
      ActionSchema{.id = "shell.showSoul", .label = "Show Soul", .category = "Shell", .capabilities = 0},
      [&](ActionActivationContext&) { longPressFired++; });

    auto const compPtr = registry.create(actionCtx, layoutNode);
    REQUIRE(compPtr != nullptr);

    auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
    REQUIRE(button != nullptr);
    CHECK(button->get_icon_name() == "emblem-system-symbolic");
    CHECK(button->has_css_class("circular"));

    emitClicked(*button);
    CHECK(primaryFired == 1);
    CHECK(longPressFired == 0);

    auto const longPressPtr = findController<Gtk::GestureLongPress>(*button);
    REQUIRE(longPressPtr);
    ::g_signal_emit_by_name(longPressPtr->gobj(), "pressed", 1.0, 1.0);
    CHECK(primaryFired == 1);
    CHECK(longPressFired == 1);

    // Gtk::Button uses the icon instead of its label when both are authored.
    auto textOnlyNode = layoutNode;
    textOnlyNode.props.erase("icon");
    auto const textOnlyCompPtr = registry.create(actionCtx, textOnlyNode);
    REQUIRE(textOnlyCompPtr != nullptr);
    auto* const textOnlyButton = dynamic_cast<Gtk::Button*>(&textOnlyCompPtr->widget());
    REQUIRE(textOnlyButton != nullptr);
    CHECK(textOnlyButton->get_label() == "Settings");
    CHECK(textOnlyButton->get_icon_name().empty());
  }

  TEST_CASE("LayoutComponentYaml - action properties expose editor metadata", "[gtk][unit][layout-component][yaml]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto const optComponentSchema = fixture.components().schema().component("actionButton");
    REQUIRE(optComponentSchema);

    auto const it = std::find_if(optComponentSchema->properties.begin(),
                                 optComponentSchema->properties.end(),
                                 [](auto const& property) { return property.name == "primaryAction"; });
    REQUIRE(it != optComponentSchema->properties.end());
    CHECK(it->kind == PropertyKind::Enum);
    CHECK(it->enumValues.empty());
    REQUIRE(it->optActionSlot);
    CHECK(*it->optActionSlot == ActionSlot::PrimaryClick);
  }

  TEST_CASE("LayoutComponentYaml - YAML boxes preserve ordered semantic children",
            "[gtk][unit][layout-component][yaml]")
  {
    auto fixture = LayoutRuntimeFixture{};

    SECTION("custom playback row keeps all six children in order")
    {
      auto const* const yaml = R"(
      type: box
      props:
        orientation: horizontal
        spacing: 4
      children:
        - type: playback.qualityIndicator
          layout:
            cssClasses: fixture-quality
        - type: playback.transportButton
          props:
            command: playPause
          layout:
            cssClasses: fixture-play-pause
        - type: playback.transportButton
          props:
            command: stop
          layout:
            cssClasses: fixture-stop
        - type: playback.seekSlider
          layout:
            hexpand: true
            cssClasses: fixture-seek
        - type: playback.timeLabel
          layout:
            cssClasses: fixture-time
        - type: playback.volumeControl
          layout:
            cssClasses: fixture-volume
    )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
      auto decodedNodeRes = readLayoutNode(tree.rootref(), "playback row fixture");
      REQUIRE(decodedNodeRes);

      auto const compPtr = fixture.create(*decodedNodeRes);
      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);
      auto const children = directChildren(*box);
      REQUIRE(children.size() == 6);
      CHECK(children[0]->has_css_class("fixture-quality"));
      CHECK(children[1]->has_css_class("fixture-play-pause"));
      CHECK(children[2]->has_css_class("fixture-stop"));
      CHECK(children[3]->has_css_class("fixture-seek"));
      CHECK(children[4]->has_css_class("fixture-time"));
      CHECK(children[5]->has_css_class("fixture-volume"));
      CHECK_FALSE(containsLayoutErrorPlaceholder(compPtr->widget()));
    }

    SECTION("minimal listening layout keeps direct and nested children in order")
    {
      auto const* const yaml = R"(
      type: box
      props:
        orientation: vertical
        spacing: 8
      children:
        - type: playback.currentTitleLabel
          layout:
            cssClasses: fixture-title
        - type: playback.currentArtistLabel
          layout:
            cssClasses: fixture-artist
        - type: playback.seekSlider
          layout:
            cssClasses: fixture-main-seek
        - type: box
          props:
            orientation: horizontal
            spacing: 4
          layout:
            cssClasses: fixture-controls
          children:
            - type: playback.transportButton
              props:
                command: playPause
              layout:
                cssClasses: fixture-controls-play-pause
            - type: playback.transportButton
              props:
                command: stop
              layout:
                cssClasses: fixture-controls-stop
            - type: playback.volumeControl
              layout:
                cssClasses: fixture-controls-volume
    )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
      auto decodedNodeRes = readLayoutNode(tree.rootref(), "listening layout fixture");
      REQUIRE(decodedNodeRes);

      auto const compPtr = fixture.create(*decodedNodeRes);
      REQUIRE(compPtr != nullptr);

      auto* const outerBox = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(outerBox != nullptr);
      auto const outerChildren = directChildren(*outerBox);
      REQUIRE(outerChildren.size() == 4);
      CHECK(outerChildren[0]->has_css_class("fixture-title"));
      CHECK(outerChildren[1]->has_css_class("fixture-artist"));
      CHECK(outerChildren[2]->has_css_class("fixture-main-seek"));
      CHECK(outerChildren[3]->has_css_class("fixture-controls"));

      auto* const controlsBox = dynamic_cast<Gtk::Box*>(outerChildren[3]);
      REQUIRE(controlsBox != nullptr);
      auto const controlsChildren = directChildren(*controlsBox);
      REQUIRE(controlsChildren.size() == 3);
      CHECK(controlsChildren[0]->has_css_class("fixture-controls-play-pause"));
      CHECK(controlsChildren[1]->has_css_class("fixture-controls-stop"));
      CHECK(controlsChildren[2]->has_css_class("fixture-controls-volume"));
      CHECK_FALSE(containsLayoutErrorPlaceholder(compPtr->widget()));
    }
  }

  TEST_CASE("LayoutComponentYaml - unknown component types produce detectable error placeholders",
            "[gtk][unit][layout-component][yaml]")
  {
    // This positive control keeps every no-placeholder assertion above and
    // below from passing because the recursive detector never fires.
    auto fixture = LayoutRuntimeFixture{};
    auto const node = LayoutNode{.type = "playback.noSuchComponent"};
    auto const compPtr = fixture.create(node);

    REQUIRE(compPtr != nullptr);
    CHECK(containsLayoutErrorPlaceholder(compPtr->widget()));
  }

  TEST_CASE("LayoutComponentYaml - full document round-trip preserves all root children",
            "[gtk][unit][layout-component][yaml]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto const* const yaml = R"(
      version: 1
      root:
        type: box
        props:
          orientation: vertical
        children:
          - type: playback.transportButton
            props:
              command: playPause
            layout:
              cssClasses: fixture-document-play-pause
          - type: playback.transportButton
            props:
              command: stop
            layout:
              cssClasses: fixture-document-stop
          - type: spacer
            layout:
              hexpand: true
              cssClasses: fixture-document-spacer
          - type: status.message
            layout:
              cssClasses: fixture-document-status
    )";

    auto tree = ryml::Tree{yaml::callbacks()};
    ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
    auto decodedRes = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
    REQUIRE(decodedRes);
    auto doc = std::move(*decodedRes);

    CHECK(doc.version == 1);
    REQUIRE(doc.root.children.size() == 4);
    CHECK(doc.root.children[0].type == "playback.transportButton");
    CHECK(doc.root.children[1].type == "playback.transportButton");
    CHECK(doc.root.children[2].type == "spacer");
    CHECK(doc.root.children[3].type == "status.message");

    auto const compPtr = fixture.layoutRuntime().build(fixture.context(), preparedLayout(doc));
    REQUIRE(compPtr != nullptr);

    auto* const rootBox = dynamic_cast<Gtk::Box*>(&compPtr->widget());
    REQUIRE(rootBox != nullptr);
    auto const children = directChildren(*rootBox);
    REQUIRE(children.size() == 4);
    CHECK(children[0]->has_css_class("fixture-document-play-pause"));
    CHECK(children[1]->has_css_class("fixture-document-stop"));
    CHECK(children[2]->has_css_class("fixture-document-spacer"));
    CHECK(children[3]->has_css_class("fixture-document-status"));
    CHECK_FALSE(containsLayoutErrorPlaceholder(compPtr->widget()));
  }

  TEST_CASE("LayoutComponentYaml - selection detail template round-trip builds without placeholders",
            "[gtk][unit][layout-component][yaml]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto const* const yaml = R"(
      version: 1
      root:
        type: template
        props:
          templateId: track.selectionDetailPane
    )";

    auto tree = ryml::Tree{yaml::callbacks()};
    ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
    auto decodedRes = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
    REQUIRE(decodedRes);
    auto doc = std::move(*decodedRes);
    doc.templates = makeDefaultLayout().templates;

    auto const compPtr = fixture.layoutRuntime().build(fixture.context(), preparedLayout(doc));

    REQUIRE(compPtr != nullptr);
    CHECK_FALSE(containsLayoutErrorPlaceholder(compPtr->widget()));
  }
} // namespace ao::gtk::layout::test
