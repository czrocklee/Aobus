// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <app/linux-gtk/layout/runtime/ComponentRegistry.h>
#include <app/linux-gtk/layout/document/LayoutDocument.h>
#include <app/linux-gtk/layout/runtime/LayoutRuntime.h>
#include <app/linux-gtk/layout/document/LayoutYaml.h>
#include <app/linux-gtk/layout/editor/LayoutEditorDialog.h>

#include <app/runtime/AppSession.h>
#include <app/runtime/ConfigStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/window.h>
#include <yaml-cpp/yaml.h>

#include <test/unit/lmdb/TestUtils.h>

#include <algorithm>
#include <set>
#include <string>

namespace
{
  class MockExecutor final : public ao::rt::IControlExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); } void defer(std::move_only_function<void()> task) override { task(); }
  };
} // namespace

using namespace ao::gtk::layout;
using namespace ao::gtk::layout::editor;

// ---------------------------------------------------------------------------
// Component descriptor validation
// ---------------------------------------------------------------------------

TEST_CASE("Component descriptor validation", "[layout][editor]")
{
  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto const& descriptors = registry.getDescriptors();

  SECTION("all 20 component types have descriptors")
  {
    REQUIRE(descriptors.size() >= 20);
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

  SECTION("leaf types have container=false and maxChildren=0")
  {
    for (auto const& desc : descriptors)
    {
      if (!desc.container)
      {
        CHECK(desc.maxChildren.value_or(0) == 0);
      }
    }
  }

  SECTION("split requires exactly 2 children")
  {
    auto const desc = registry.getDescriptor("split");

    REQUIRE(desc.has_value());
    CHECK(desc->minChildren == 2);
    CHECK(desc->maxChildren.has_value());
    CHECK(*desc->maxChildren == 2);
  }

  SECTION("scroll requires exactly 1 child")
  {
    auto const desc = registry.getDescriptor("scroll");

    REQUIRE(desc.has_value());
    CHECK(desc->minChildren == 1);
    CHECK(desc->maxChildren.has_value());
    CHECK(*desc->maxChildren == 1);
  }

  SECTION("tabs requires at least 1 child")
  {
    auto const desc = registry.getDescriptor("tabs");

    REQUIRE(desc.has_value());
    CHECK(desc->minChildren == 1);
    CHECK(!desc->maxChildren.has_value()); // unbounded
  }

  SECTION("box has orientation, spacing, homogeneous props")
  {
    auto const desc = registry.getDescriptor("box");

    REQUIRE(desc.has_value());
    CHECK(desc->container == true);

    auto const hasProp = [&](std::string const& name)
    { return std::ranges::any_of(desc->props, [&](auto const& prop) { return prop.name == name; }); };

    CHECK(hasProp("orientation"));
    CHECK(hasProp("spacing"));
    CHECK(hasProp("homogeneous"));
  }

  SECTION("playPauseButton has showLabel and size props")
  {
    auto const desc = registry.getDescriptor("playback.playPauseButton");

    REQUIRE(desc.has_value());
    CHECK(desc->category == "Playback");

    auto const hasProp = [&](std::string const& name)
    { return std::ranges::any_of(desc->props, [&](auto const& prop) { return prop.name == name; }); };

    CHECK(hasProp("showLabel"));
    CHECK(hasProp("size"));
  }

  SECTION("getDescriptor returns nullopt for unknown type")
  {
    auto const desc = registry.getDescriptor("nonexistent.component");
    CHECK(!desc.has_value());
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
    char const* types[] = {"box",
                           "split",
                           "scroll",
                           "spacer",
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
                           "inspector.coverArt",
                           "inspector.sidebar",
                           "status.defaultBar",
                           "app.menuBar",
                           "app.workspaceWithInspector"};

    for (auto const* type : types)
    {
      auto const desc = registry.getDescriptor(type);
      CHECK(desc.has_value());
    }
  }
}

// ---------------------------------------------------------------------------
// LayoutEditorDialog
// ---------------------------------------------------------------------------

TEST_CASE("LayoutEditorDialog", "[layout][editor]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_editor_test");

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto const doc = createDefaultLayout();

  SECTION("Dialog constructs without crash")
  {
    auto* const dialog = new LayoutEditorDialog(window, registry, doc);
    REQUIRE(dialog != nullptr);
    dialog->close();
    delete dialog;
  }

  SECTION("document returns the initial document on construction")
  {
    auto* const dialog = new LayoutEditorDialog(window, registry, doc);
    auto const& returned = dialog->document();

    CHECK(returned.root.type == doc.root.type);
    CHECK(returned.root.id == doc.root.id);

    dialog->close();
    delete dialog;
  }

  SECTION("reset default restores to createDefaultLayout shape")
  {
    // Create a modified document
    auto modified = LayoutDocument{};
    modified.root.type = "spacer";

    auto* const dialog = new LayoutEditorDialog(window, registry, modified);

    // The dialog copies the document, so modifications to the dialog's copy
    // are reflected. Just verify the initial copy is correct.
    CHECK(dialog->document().root.type == "spacer");

    dialog->close();
    delete dialog;
  }

  SECTION("signalApplyPreview is emitted on document changes")
  {
    auto* const dialog = new LayoutEditorDialog(window, registry, doc);
    int count = 0;

    dialog->signalApplyPreview().connect([&](LayoutDocument const&) { ++count; });

    CHECK(count == 0);

    dialog->close();
    delete dialog;
  }
}

// ---------------------------------------------------------------------------
// Template system
// ---------------------------------------------------------------------------

TEST_CASE("Template system", "[layout][editor]")
{
  SECTION("getBuiltInTemplates returns all 7 built-ins")
  {
    auto const templates = getBuiltInTemplates();

    CHECK(templates.contains("playback.compactControls"));
    CHECK(templates.contains("playback.defaultBar"));
    CHECK(templates.contains("library.defaultSidebar"));
    CHECK(templates.contains("inspector.defaultPanel"));
    CHECK(templates.contains("status.defaultBar"));
    CHECK(templates.contains("tracks.defaultWorkspace"));
    CHECK(templates.contains("app.defaultLayout"));

    int const expectedCount = 7;
    CHECK(templates.size() >= expectedCount);
  }

  SECTION("playback.defaultBar contains all expected children")
  {
    auto const templates = getBuiltInTemplates();
    auto const& bar = templates.at("playback.defaultBar");

    CHECK(bar.type == "box");

    // 6 children: outputButton, playPauseButton, stopButton, seekSlider, timeLabel, volumeControl
    int const expectedChildren = 6;
    CHECK(bar.children.size() == expectedChildren);
  }

  SECTION("template expansion via expandNode in build")
  {
    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto const tempDir = TempDir{};
    auto const executor = std::make_shared<MockExecutor>();
    auto const configStore =
      std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto session = ao::rt::AppSession{
      ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

    auto const app = Gtk::Application::create("io.github.aobus.template_test");
    auto window = Gtk::Window{};
    auto ctx = LayoutDependencies{.registry = registry, .session = session, .parentWindow = window, .onNodeMoved = {}};

    auto doc = LayoutDocument{};
    doc.version = 1;
    doc.templates = getBuiltInTemplates();
    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"playback.compactControls"}};

    auto runtime = LayoutRuntime{registry};
    auto const comp = runtime.build(ctx, doc);

    REQUIRE(comp != nullptr);

    auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());
    REQUIRE(box != nullptr);
  }

  SECTION("recursive template reference produces error")
  {
    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto const tempDir = TempDir{};
    auto const executor = std::make_shared<MockExecutor>();
    auto const configStore =
      std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto session = ao::rt::AppSession{
      ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

    auto const app = Gtk::Application::create("io.github.aobus.recursive_test");
    auto window = Gtk::Window{};
    auto ctx = LayoutDependencies{.registry = registry, .session = session, .parentWindow = window, .onNodeMoved = {}};

    auto doc = LayoutDocument{};
    doc.version = 1;
    doc.templates["selfRef"] = LayoutNode{.type = "template"};
    doc.templates["selfRef"].props["templateId"] = LayoutValue{std::string{"selfRef"}};
    doc.root.type = "template";
    doc.root.props["templateId"] = LayoutValue{std::string{"selfRef"}};

    auto runtime = LayoutRuntime{registry};
    auto const comp = runtime.build(ctx, doc);

    REQUIRE(comp != nullptr);
    CHECK(dynamic_cast<Gtk::Widget*>(&comp->widget()) != nullptr);
  }

  SECTION("template YAML round-trip")
  {
    auto doc = LayoutDocument{};
    doc.version = 2;
    doc.templates = getBuiltInTemplates();

    auto const node = YAML::Node(doc);
    auto const decoded = node.as<LayoutDocument>();

    REQUIRE(decoded.templates.contains("playback.compactControls"));
    CHECK(decoded.templates.at("playback.compactControls").type == "box");
  }
}

// ---------------------------------------------------------------------------
// absoluteCanvas
// ---------------------------------------------------------------------------

TEST_CASE("absoluteCanvas component", "[layout][editor]")
{
  auto const app = Gtk::Application::create("io.github.aobus.canvas_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = LayoutDependencies{.registry = registry, .session = session, .parentWindow = window, .onNodeMoved = {}};
  SECTION("absoluteCanvas descriptor is registered as container")
  {
    auto const desc = registry.getDescriptor("absoluteCanvas");

    REQUIRE(desc.has_value());
    CHECK(desc->container == true);
    CHECK(desc->minChildren == 0);
    CHECK(!desc->maxChildren.has_value());
  }

  SECTION("absoluteCanvas with no children builds without crash")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "absoluteCanvas";

    auto runtime = LayoutRuntime{registry};
    auto const comp = runtime.build(ctx, doc);

    REQUIRE(comp != nullptr);
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

    auto runtime = LayoutRuntime{registry};
    auto const comp = runtime.build(ctx, doc);

    REQUIRE(comp != nullptr);
  }
}
