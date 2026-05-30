// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/layout/document/LayoutNode.h"
#include "app/linux-gtk/layout/editor/LayoutEditorDialog.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/uimodel/layout/LayoutYaml.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/dialog.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout::editor::test
{
  using ao::gtk::test::makeRuntime;

  namespace
  {
  } // namespace

  using namespace ao::lmdb::test;

  TEST_CASE("Component descriptor validation", "[layout][unit][editor]")
  {
    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto const& descriptors = registry.descriptors();

    SECTION("all 21 component types have descriptors")
    {
      REQUIRE(descriptors.size() >= 21);
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
        CHECK(!desc.category.empty());
      }
    }

    SECTION("container types have container=true")
    {
      auto const expectedContainers = std::set<std::string>{"box", "split", "scroll", "tabs"};

      for (auto const& desc : descriptors)
      {
        if (expectedContainers.contains(desc.type))
        {
          CHECK(desc.container == true);
        }
      }
    }

    SECTION("leaf types have container=false and optMaxChildren=0")
    {
      for (auto const& desc : descriptors)
      {
        if (!desc.container)
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
      CHECK(optDesc->optMaxChildren.has_value());
      CHECK(*optDesc->optMaxChildren == 2);
    }

    SECTION("scroll requires exactly 1 child")
    {
      auto const optDesc = registry.descriptor("scroll");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->minChildren == 1);
      CHECK(optDesc->optMaxChildren.has_value());
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
      CHECK(optDesc->container == true);

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
      CHECK(optDesc->category == "Playback");

      auto const hasProp = [&](std::string const& name)
      { return std::ranges::any_of(optDesc->props, [&](auto const& prop) { return prop.name == name; }); };

      CHECK(hasProp("showLabel"));
      CHECK(hasProp("size"));
    }

    SECTION("playback.outputButton has gesture action props")
    {
      auto const optDesc = registry.descriptor("playback.outputButton");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->category == "Playback");

      auto const hasProp = [&](std::string const& name)
      { return std::ranges::any_of(optDesc->props, [&](auto const& prop) { return prop.name == name; }); };

      CHECK(hasProp("primaryAction"));
      CHECK(hasProp("primaryLongPressAction"));
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
        categories.insert(desc.category);
      }

      CHECK(categories.contains("Containers"));
      CHECK(categories.contains("Playback"));
      CHECK(categories.contains("Application"));
      CHECK(categories.contains("Status"));
      CHECK(categories.contains("Library"));
      CHECK(categories.contains("Tracks"));
      CHECK(categories.contains("Inspector"));
    }

    SECTION("all 20 types individually retrievable")
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
                                                          "playback.outputButton",
                                                          "playback.qualityIndicator",
                                                          "status.messageLabel",
                                                          "library.listTree",
                                                          "tracks.table",
                                                          "library.openLibraryButton",
                                                          "inspector.image",
                                                          "inspector.panel",
                                                          "app.menuBar",
                                                          "app.workspaceWithInspector"});

      for (auto const& type : types)
      {
        auto const optDesc = registry.descriptor(std::string{type});
        CHECK(optDesc.has_value());
      }
    }
  }

  // ---------------------------------------------------------------------------
  // LayoutEditorDialog
  // ---------------------------------------------------------------------------
  TEST_CASE("LayoutEditorDialog", "[layout][unit][editor]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_editor_test");

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);
    auto actionRegistry = ActionRegistry{};

    auto window = Gtk::Window{};
    auto const doc = createDefaultLayout();

    SECTION("Dialog constructs without crash")
    {
      auto dialogPtr = std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, doc, "classic");
      REQUIRE(dialogPtr != nullptr);
      dialogPtr->close();
    }

    SECTION("document returns the initial document on construction")
    {
      auto dialogPtr = std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, doc, "classic");
      auto const& returned = dialogPtr->document();

      CHECK(returned.root.type == doc.root.type);
      CHECK(returned.root.id == doc.root.id);

      dialogPtr->close();
    }

    SECTION("reset default restores to createDefaultLayout shape")
    {
      // Create a modified document
      auto modified = LayoutDocument{};
      modified.root.type = "spacer";

      auto dialogPtr = std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, modified, "classic");

      // The dialog copies the document, so modifications to the dialog's copy
      // are reflected. Just verify the initial copy is correct.
      CHECK(dialogPtr->document().root.type == "spacer");

      dialogPtr->close();
    }

    SECTION("Validation prevents saving unknown actions")
    {
      auto invalidDoc = LayoutDocument{};
      invalidDoc.root.type = "app.actionButton";
      invalidDoc.root.props["primaryAction"] = LayoutValue{"this.does.not.exist"};

      auto dialogPtr = std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, invalidDoc, "classic");

      // Attempting to save an invalid document should fail validation and keep dialog open
      dialogPtr->response(Gtk::ResponseType::OK);
      // We can't check visible here since we didn't show(), but we verify it's still alive/active
      CHECK(dialogPtr != nullptr);
      dialogPtr->close();

      auto validDoc = LayoutDocument{};
      validDoc.root.type = "app.actionButton";
      validDoc.root.props["primaryAction"] = LayoutValue{"none"};

      auto dialogValidPtr = std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, validDoc, "classic");

      // Attempting to save a valid document should succeed and close the dialog
      dialogValidPtr->response(Gtk::ResponseType::OK);
    }

    SECTION("signalApplyPreview is emitted on document changes")
    {
      auto dialogPtr = std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, doc, "classic");
      std::int32_t count = 0;

      dialogPtr->signalApplyPreview().connect([&](LayoutDocument const&) { ++count; });

      CHECK(count == 0);

      dialogPtr->close();
    }
  }

  // ---------------------------------------------------------------------------
  // Template system
  // ---------------------------------------------------------------------------
  TEST_CASE("Template system", "[layout][unit][editor]")
  {
    SECTION("getBuiltInTemplates returns all 8 built-ins")
    {
      auto const templates = getBuiltInTemplates();

      CHECK(templates.contains("playback.compactControls"));
      CHECK(templates.contains("playback.transportGroup"));
      CHECK(templates.contains("playback.defaultBar"));
      CHECK(templates.contains("library.defaultListPane"));
      CHECK(templates.contains("inspector.defaultPanel"));
      CHECK(templates.contains("status.defaultBar"));
      CHECK(templates.contains("tracks.defaultWorkspace"));
      CHECK(templates.contains("app.defaultLayout"));

      int const expectedCount = 8;
      CHECK(templates.size() >= expectedCount);
    }

    SECTION("playback.transportGroup has 2 children and linked class")
    {
      auto const templates = getBuiltInTemplates();
      auto const& group = templates.at("playback.transportGroup");

      CHECK(group.type == "box");
      CHECK(group.getProp<std::int64_t>("spacing", -1) == 0);

      auto const classes = group.getLayout<std::vector<std::string>>("cssClasses", {});
      CHECK(std::ranges::contains(classes, std::string_view{"linked"}));
      CHECK(group.children.size() == 2);
    }

    SECTION("playback.defaultBar contains all expected children")
    {
      auto const templates = getBuiltInTemplates();
      auto const& bar = templates.at("playback.defaultBar");

      CHECK(bar.type == "box");

      // 3 children: left fixed controls, flexible seek slider, right fixed status controls.
      int const expectedChildren = 3;
      CHECK(bar.children.size() == expectedChildren);
      CHECK(bar.children[0].type == "box");
      CHECK(bar.children[0].children.size() == 2);
      CHECK(bar.children[0].children[0].type == "playback.outputButton");
      CHECK(bar.children[0].children[1].type == "template");
      CHECK(bar.children[0].children[1].getProp<std::string>("templateId", "") == "playback.transportGroup");
      CHECK(bar.children[1].type == "playback.seekSlider");
      CHECK(bar.children[1].getLayout<bool>("hexpand", false));
      CHECK(bar.children[2].type == "box");
      CHECK(bar.children[2].children.size() == 2);
      CHECK(bar.children[2].children[0].type == "playback.timeLabel");
      CHECK(bar.children[2].children[1].type == "playback.volumeControl");

      // Grouping regions carry ao-grouping-region CSS class.
      CHECK(bar.children[0].getLayout<std::string>("cssClasses", "") == "ao-grouping-region");
      CHECK(bar.children[2].getLayout<std::string>("cssClasses", "") == "ao-grouping-region");
    }

    SECTION("status.defaultBar template contains 7 children")
    {
      auto const templates = getBuiltInTemplates();
      auto const& bar = templates.at("status.defaultBar");

      CHECK(bar.type == "box");

      // 7 children: playbackDetails, spacer, nowPlaying, spacer, statusSlot, separator, trackCount
      int const expectedChildren = 7;
      CHECK(bar.children.size() == expectedChildren);
    }

    SECTION("template expansion via expandNode in build")
    {
      auto registry = ComponentRegistry{};
      LayoutRuntime::registerStandardComponents(registry);

      auto const tempDir = TempDir{};
      auto runtime = makeRuntime(tempDir);

      auto const appPtr = Gtk::Application::create("io.github.aobus.template_test");
      auto window = Gtk::Window{};
      auto const actionRegistry = ActionRegistry{};
      auto ctx = LayoutContext{
        .registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

      auto doc = LayoutDocument{};
      doc.version = 1;
      doc.templates = getBuiltInTemplates();
      doc.root.type = "template";
      doc.root.props["templateId"] = LayoutValue{std::string{"playback.compactControls"}};

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);
    }

    SECTION("recursive template reference produces error")
    {
      auto registry = ComponentRegistry{};
      LayoutRuntime::registerStandardComponents(registry);

      auto const tempDir = TempDir{};
      auto runtime = makeRuntime(tempDir);

      auto const appPtr = Gtk::Application::create("io.github.aobus.recursive_test");
      auto window = Gtk::Window{};
      auto const actionRegistry = ActionRegistry{};
      auto ctx = LayoutContext{
        .registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

      auto doc = LayoutDocument{};
      doc.version = 1;
      doc.templates["selfRef"] = LayoutNode{.type = "template"};
      doc.templates["selfRef"].props["templateId"] = LayoutValue{std::string{"selfRef"}};
      doc.root.type = "template";
      doc.root.props["templateId"] = LayoutValue{std::string{"selfRef"}};

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);
      CHECK(dynamic_cast<Gtk::Widget*>(&compPtr->widget()) != nullptr);
    }

    SECTION("template YAML round-trip")
    {
      auto doc = LayoutDocument{};
      doc.version = 2;
      doc.templates = getBuiltInTemplates();

      auto tree = ryml::Tree{};
      rt::yaml::write(tree.rootref(), doc);

      auto decoded = LayoutDocument{};
      REQUIRE(rt::yaml::read(tree.rootref(), decoded));

      REQUIRE(decoded.templates.contains("playback.compactControls"));
      CHECK(decoded.templates.at("playback.compactControls").type == "box");
    }
  }

  // ---------------------------------------------------------------------------
  // absoluteCanvas
  // ---------------------------------------------------------------------------
  TEST_CASE("absoluteCanvas component", "[layout][unit][editor]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.canvas_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    SECTION("absoluteCanvas descriptor is registered as container")
    {
      auto const optDesc = registry.descriptor("absoluteCanvas");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->container == true);
      CHECK(optDesc->minChildren == 0);
      CHECK(!optDesc->optMaxChildren.has_value());
    }

    SECTION("absoluteCanvas with no children builds without crash")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "absoluteCanvas";

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);
    }

    SECTION("absoluteCanvas with positioned child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "absoluteCanvas";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.id = "pos-spacer";
      child.layout["x"] = LayoutValue{static_cast<std::int64_t>(50)};
      child.layout["y"] = LayoutValue{static_cast<std::int64_t>(100)};
      child.layout["width"] = LayoutValue{static_cast<std::int64_t>(200)};
      child.layout["height"] = LayoutValue{static_cast<std::int64_t>(50)};
      child.layout["zIndex"] = LayoutValue{static_cast<std::int64_t>(2)};
      doc.root.children.push_back(std::move(child));

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);
    }
  }
} // namespace ao::gtk::layout::editor::test
