// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/app/GtkUiDependencies.h"
#include "app/linux-gtk/layout/document/LayoutPresets.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutBuildContext.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>
#include <ao/uimodel/layout/shell/LayoutRuntimeState.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/button.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::emitClicked;

  TEST_CASE("LayoutComponentYaml - YAML semantic layout documents build GTK components",
            "[gtk][unit][layout-component][yaml]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& registry = fixture.components();

    SECTION("actionButton builds from YAML and binds actions")
    {
      auto actionRegistry = ActionRegistry{};
      auto runtimeState = uimodel::LayoutRuntimeState{};
      auto dependencies = GtkUiDependencies{.outputDeviceIntent = uimodel::OutputDeviceIntent::discarded()};
      auto actionCtx = LayoutBuildContext{.registry = registry,
                                          .actionRegistry = actionRegistry,
                                          .runtime = fixture.runtime(),
                                          .parentWindow = fixture.window(),
                                          .runtimeState = runtimeState,
                                          .buildState = uimodel::LayoutBuildStateView{runtimeState},
                                          .dependencies = dependencies};
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

      actionRegistry.registerAction(LayoutActionDescriptor{.id = "shell.showSystemMenu",
                                                           .label = "System Menu",
                                                           .category = "Shell",
                                                           .capabilities = LayoutActionCapability::None},
                                    [&](ActionActivationContext&) { primaryFired++; });

      actionRegistry.registerAction(LayoutActionDescriptor{.id = "shell.showSoul",
                                                           .label = "Show Soul",
                                                           .category = "Shell",
                                                           .capabilities = LayoutActionCapability::None},
                                    [&](ActionActivationContext&) { longPressFired++; });

      auto const compPtr = registry.create(actionCtx, layoutNode);
      REQUIRE(compPtr != nullptr);

      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      CHECK(button->get_icon_name() == "emblem-system-symbolic");
      CHECK(button->has_css_class("circular"));

      // Verify that clicking the button routes primary action through the registry
      emitClicked(*button);
      CHECK(primaryFired == 1);
      CHECK(longPressFired == 0);
    }

    SECTION("actionButton exposes enum properties for editor")
    {
      auto const optDesc = registry.descriptor("actionButton");
      REQUIRE(optDesc);

      auto const it = std::find_if(
        optDesc->props.begin(), optDesc->props.end(), [](auto const& p) { return p.name == "primaryAction"; });
      REQUIRE(it != optDesc->props.end());
      CHECK(it->kind == LayoutPropertyKind::Enum);
      CHECK(it->enumValues.empty());
      REQUIRE(it->optActionBinding);
      CHECK(it->optActionBinding->slot == LayoutActionSlot::PrimaryClick);
    }

    SECTION("custom playback row YAML builds without errors")
    {
      auto const* const yaml = R"(
      type: box
      props:
        orientation: horizontal
        spacing: 4
      children:
        - type: playback.qualityIndicator
        - type: playback.transportButton
          props:
            command: playPause
        - type: playback.transportButton
          props:
            command: stop
        - type: playback.seekSlider
          layout:
            hexpand: true
        - type: playback.timeLabel
        - type: playback.volumeControl
    )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
      auto decodedNodeRes = readLayoutNode(tree.rootref(), "playback row fixture");
      REQUIRE(decodedNodeRes);
      auto layoutNode = std::move(*decodedNodeRes);

      auto const compPtr = fixture.create(layoutNode);
      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);

      auto* const child = box->get_first_child();
      CHECK(child != nullptr);
      CHECK_FALSE(containsLayoutErrorPlaceholder(compPtr->widget()));
    }

    SECTION("the placeholder check answers yes for a type no shell registers")
    {
      // Without this, every "builds without errors" section above could be
      // passing because the check never fires, rather than because the
      // document is sound.
      auto const node = uimodel::LayoutNode{.type = "playback.noSuchComponent"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      CHECK(containsLayoutErrorPlaceholder(compPtr->widget()));
    }

    SECTION("minimal listening layout YAML builds without errors")
    {
      auto const* const yaml = R"(
      type: box
      props:
        orientation: vertical
        spacing: 8
      children:
        - type: playback.currentTitleLabel
        - type: playback.currentArtistLabel
        - type: playback.seekSlider
        - type: box
          props:
            orientation: horizontal
            spacing: 4
          children:
            - type: playback.transportButton
              props:
                command: playPause
            - type: playback.transportButton
              props:
                command: stop
            - type: playback.volumeControl
    )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
      auto decodedNodeRes = readLayoutNode(tree.rootref(), "listening layout fixture");
      REQUIRE(decodedNodeRes);
      auto layoutNode = std::move(*decodedNodeRes);

      auto const compPtr = fixture.create(layoutNode);
      REQUIRE(compPtr != nullptr);

      auto* const outerBox = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      CHECK(outerBox != nullptr);
      CHECK_FALSE(containsLayoutErrorPlaceholder(compPtr->widget()));
    }

    SECTION("full layout document round-trip then build")
    {
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
          - type: playback.transportButton
            props:
              command: stop
          - type: spacer
            layout:
              hexpand: true
          - type: status.message
    )";

      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
      auto decodedRes = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
      REQUIRE(decodedRes);
      auto doc = std::move(*decodedRes);

      CHECK(doc.version == 1);
      CHECK(doc.root.children.size() == 4);

      auto const compPtr = fixture.layoutRuntime().build(ctx, preparedLayout(doc));

      REQUIRE(compPtr != nullptr);
      CHECK_FALSE(containsLayoutErrorPlaceholder(compPtr->widget()));
    }

    SECTION("track.selectionDetailPane template round-trip then build")
    {
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

      auto const compPtr = fixture.layoutRuntime().build(ctx, preparedLayout(doc));

      CHECK(compPtr != nullptr);
    }
  }
} // namespace ao::gtk::layout::test
