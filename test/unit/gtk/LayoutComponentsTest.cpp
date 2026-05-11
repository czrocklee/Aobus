// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <app/linux-gtk/layout/ComponentRegistry.h>
#include <app/linux-gtk/layout/LayoutDocument.h>
#include <app/linux-gtk/layout/LayoutRuntime.h>
#include <app/linux-gtk/layout/LayoutYaml.h>
#include <app/linux-gtk/layout/components/Containers.h>

#include <CoverArtCache.h>
#include <StatusBar.h>
#include <TrackRowDataProvider.h>
#include <app/runtime/AppSession.h>
#include <app/runtime/ConfigStore.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/menu.h>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/popovermenubar.h>
#include <gtkmm/scale.h>
#include <gtkmm/window.h>
#include <yaml-cpp/yaml.h>

#include <test/unit/lmdb/TestUtils.h>

using namespace ao::gtk::layout;

namespace
{
  class MockExecutor final : public ao::rt::IControlExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); }
  };

  ComponentContext makeContext(ComponentRegistry& registry, ao::rt::AppSession& session, Gtk::Window& window)
  {
    return ComponentContext{.registry = registry, .session = session, .parentWindow = window};
  }
} // namespace

// ---------------------------------------------------------------------------
// Playback components
// ---------------------------------------------------------------------------

TEST_CASE("Playback component instantiation", "[layout][components]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = makeContext(registry, session, window);

  SECTION("playPauseButton creates Gtk::Button")
  {
    auto const node = LayoutNode{.type = "playback.playPauseButton"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const btn = dynamic_cast<Gtk::Button*>(&comp->widget());
    REQUIRE(btn != nullptr);
    CHECK(btn->get_icon_name() == "media-playback-start-symbolic");
    CHECK(btn->get_sensitive() == false);
  }

  SECTION("stopButton creates Gtk::Button, insensitive when idle")
  {
    auto const node = LayoutNode{.type = "playback.stopButton"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const btn = dynamic_cast<Gtk::Button*>(&comp->widget());
    REQUIRE(btn != nullptr);
    CHECK(btn->get_icon_name() == "media-playback-stop-symbolic");
    CHECK(btn->get_sensitive() == false);
  }

  SECTION("playButton creates Gtk::Button, insensitive when not ready")
  {
    auto const node = LayoutNode{.type = "playback.playButton"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const btn = dynamic_cast<Gtk::Button*>(&comp->widget());
    REQUIRE(btn != nullptr);
    CHECK(btn->get_icon_name() == "media-playback-start-symbolic");
    CHECK(btn->get_sensitive() == false);
  }

  SECTION("pauseButton creates Gtk::Button, insensitive when not playing")
  {
    auto const node = LayoutNode{.type = "playback.pauseButton"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const btn = dynamic_cast<Gtk::Button*>(&comp->widget());
    REQUIRE(btn != nullptr);
    CHECK(btn->get_icon_name() == "media-playback-pause-symbolic");
    CHECK(btn->get_sensitive() == false);
  }

  SECTION("seekSlider creates Gtk::Scale, insensitive when idle")
  {
    auto const node = LayoutNode{.type = "playback.seekSlider"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const scale = dynamic_cast<Gtk::Scale*>(&comp->widget());
    REQUIRE(scale != nullptr);
    CHECK(scale->get_sensitive() == false);
    CHECK(scale->get_value() == 0.0);
  }

  SECTION("timeLabel creates Gtk::Label with default text")
  {
    auto const node = LayoutNode{.type = "playback.timeLabel"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_text() == "00:00 / 00:00");
  }

  SECTION("currentTitleLabel shows Not Playing when idle")
  {
    auto const node = LayoutNode{.type = "playback.currentTitleLabel"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_text() == "Not Playing");
  }

  SECTION("currentArtistLabel shows empty when idle")
  {
    auto const node = LayoutNode{.type = "playback.currentArtistLabel"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_text() == "");
  }

  SECTION("volumeControl shows hidden when volume unavailable")
  {
    auto const node = LayoutNode{.type = "playback.volumeControl"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);
    CHECK(comp->widget().get_visible() == false);
  }

  SECTION("outputButton creates Gtk::Button with AobusSoul child")
  {
    auto const node = LayoutNode{.type = "playback.outputButton"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const btn = dynamic_cast<Gtk::Button*>(&comp->widget());
    REQUIRE(btn != nullptr);
    CHECK(btn->has_css_class("output-button-logo"));
  }

  SECTION("qualityIndicator creates AobusSoul widget")
  {
    auto const node = LayoutNode{.type = "playback.qualityIndicator"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    int width = -1;
    int height = -1;
    comp->widget().get_size_request(width, height);

    int const expectedSize = 24;
    CHECK(width == expectedSize);
    CHECK(height == expectedSize);
  }

  SECTION("all 11 playback types register and instantiate")
  {
    char const* types[] = {"playback.playPauseButton",
                           "playback.stopButton",
                           "playback.volumeControl",
                           "playback.currentTitleLabel",
                           "playback.currentArtistLabel",
                           "playback.seekSlider",
                           "playback.timeLabel",
                           "playback.playButton",
                           "playback.pauseButton",
                           "playback.outputButton",
                           "playback.qualityIndicator"};

    for (auto const* type : types)
    {
      auto const node = LayoutNode{.type = type};
      auto const comp = registry.create(ctx, node);
      CHECK(comp != nullptr);
    }
  }
}

// ---------------------------------------------------------------------------
// Semantic components — error states
// ---------------------------------------------------------------------------

TEST_CASE("Semantic component error states", "[layout][components]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = makeContext(registry, session, window);

  SECTION("library.listTree shows error when rowDataProvider missing")
  {
    auto const node = LayoutNode{.type = "library.listTree"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_label().find("rowDataProvider missing") != std::string::npos);
  }

  SECTION("library.listTree shows error when listSidebarController missing")
  {
    auto const rdp = std::make_unique<ao::gtk::TrackRowDataProvider>(session.musicLibrary());
    ctx.rowDataProvider = rdp.get();
    auto const node = LayoutNode{.type = "library.listTree"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_label().find("listSidebarController missing") != std::string::npos);
  }

  SECTION("tracks.table shows error when trackPageGraph missing")
  {
    auto const node = LayoutNode{.type = "tracks.table"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());
    REQUIRE(box != nullptr);

    auto* const child = box->get_first_child();
    REQUIRE(child != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(child);
    REQUIRE(label != nullptr);
    CHECK(label->get_label().find("trackPageGraph missing") != std::string::npos);
  }

  SECTION("inspector.coverArt shows error when coverArtCache missing")
  {
    auto const node = LayoutNode{.type = "inspector.coverArt"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_label().find("coverArtCache missing") != std::string::npos);
  }

  SECTION("inspector.sidebar shows error when coverArtCache missing")
  {
    auto const node = LayoutNode{.type = "inspector.sidebar"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_label().find("coverArtCache missing") != std::string::npos);
  }

  SECTION("status.defaultBar shows error when statusBar missing")
  {
    auto const node = LayoutNode{.type = "status.defaultBar"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_label().find("statusBar missing") != std::string::npos);
  }

  SECTION("app.workspaceWithInspector shows error when trackPageGraph missing")
  {
    auto const node = LayoutNode{.type = "app.workspaceWithInspector"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());
    REQUIRE(box != nullptr);

    auto* const child = box->get_first_child();
    REQUIRE(child != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(child);
    REQUIRE(label != nullptr);
    CHECK(label->get_label().find("trackPageGraph missing") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Semantic components — success states
// ---------------------------------------------------------------------------

TEST_CASE("Semantic component success states", "[layout][components]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};

  int const cacheSize = 10;
  auto coverArtCache = std::make_unique<ao::gtk::CoverArtCache>(cacheSize);
  auto statusBar = std::make_unique<ao::gtk::StatusBar>(session);
  auto menuModel = Gio::Menu::create();
  menuModel->append("Test Item", "win.test");

  auto ctx = ComponentContext{.registry = registry,
                              .session = session,
                              .parentWindow = window,
                              .coverArtCache = coverArtCache.get(),
                              .statusBar = statusBar.get(),
                              .menuModel = menuModel,
                              .onNodeMoved = {}};

  [[maybe_unused]] auto runtime = LayoutRuntime{registry};
  {
    auto const node = LayoutNode{.type = "status.messageLabel"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_text() == "Aobus Ready");
  }

  SECTION("library.openLibraryButton creates Gtk::Button")
  {
    auto const node = LayoutNode{.type = "library.openLibraryButton"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const btn = dynamic_cast<Gtk::Button*>(&comp->widget());
    REQUIRE(btn != nullptr);
    CHECK(btn->get_icon_name() == "folder-open-symbolic");
  }

  SECTION("app.menuBar creates Gtk::PopoverMenuBar")
  {
    auto const node = LayoutNode{.type = "app.menuBar"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const menuBar = dynamic_cast<Gtk::PopoverMenuBar*>(&comp->widget());
    REQUIRE(menuBar != nullptr);
  }

  SECTION("app.menuBar tolerates absent menu model")
  {
    auto ctx2 = makeContext(registry, session, window);
    auto const node = LayoutNode{.type = "app.menuBar"};
    auto const comp = registry.create(ctx2, node);

    REQUIRE(comp != nullptr);
    CHECK(dynamic_cast<Gtk::PopoverMenuBar*>(&comp->widget()) != nullptr);
  }

  SECTION("inspector.coverArt creates CoverArtWidget when cache available")
  {
    auto const node = LayoutNode{.type = "inspector.coverArt"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    CHECK(label == nullptr);
  }

  SECTION("inspector.sidebar creates InspectorSidebar when cache available")
  {
    auto const node = LayoutNode{.type = "inspector.sidebar"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);

    auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
    CHECK(label == nullptr);
  }

  SECTION("status.defaultBar returns the context statusBar widget")
  {
    auto const node = LayoutNode{.type = "status.defaultBar"};
    auto const comp = registry.create(ctx, node);

    REQUIRE(comp != nullptr);
    CHECK(&comp->widget() == statusBar.get());
  }
}

// ---------------------------------------------------------------------------
// All types registration
// ---------------------------------------------------------------------------

TEST_CASE("All component types register and instantiate", "[layout][components]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = makeContext(registry, session, window);

  SECTION("all 9 semantic types")
  {
    char const* types[] = {"status.messageLabel",
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
      auto const node = LayoutNode{.type = type};
      auto const comp = registry.create(ctx, node);
      CHECK(comp != nullptr);
    }
  }
}

// ---------------------------------------------------------------------------
// YAML round-trip with semantic components
// ---------------------------------------------------------------------------

TEST_CASE("YAML layout with semantic components", "[layout][components]")
{
  auto const app = Gtk::Application::create("io.github.aobus.layout_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  auto registry = ComponentRegistry{};
  LayoutRuntime::registerStandardComponents(registry);

  auto window = Gtk::Window{};
  auto ctx = makeContext(registry, session, window);

  SECTION("custom playback row YAML builds without errors")
  {
    auto const node = YAML::Load(R"(
      type: box
      props:
        orientation: horizontal
        spacing: 4
      children:
        - type: playback.outputButton
        - type: playback.playPauseButton
        - type: playback.stopButton
        - type: playback.seekSlider
          layout:
            hexpand: true
        - type: playback.timeLabel
        - type: playback.volumeControl
    )");
    auto const layoutNode = node.as<LayoutNode>();

    auto const comp = registry.create(ctx, layoutNode);
    REQUIRE(comp != nullptr);

    auto* const box = dynamic_cast<Gtk::Box*>(&comp->widget());
    REQUIRE(box != nullptr);

    auto* const child = box->get_first_child();
    REQUIRE(child != nullptr);
  }

  SECTION("minimal listening layout YAML builds without errors")
  {
    auto const node = YAML::Load(R"(
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
            - type: playback.playPauseButton
            - type: playback.stopButton
            - type: playback.volumeControl
    )");
    auto const layoutNode = node.as<LayoutNode>();

    auto const comp = registry.create(ctx, layoutNode);
    REQUIRE(comp != nullptr);

    auto* const outerBox = dynamic_cast<Gtk::Box*>(&comp->widget());
    REQUIRE(outerBox != nullptr);
  }

  SECTION("full layout document round-trip then build")
  {
    auto const yaml = R"(
      version: 1
      root:
        type: box
        props:
          orientation: vertical
        children:
          - type: playback.playPauseButton
          - type: playback.stopButton
          - type: spacer
            layout:
              hexpand: true
          - type: status.messageLabel
    )";

    auto const parsed = YAML::Load(yaml);
    auto const doc = parsed.as<LayoutDocument>();

    CHECK(doc.version == 1);
    CHECK(doc.root.children.size() == 4);

    auto runtime = LayoutRuntime{registry};
    auto const comp = runtime.build(ctx, doc);

    REQUIRE(comp != nullptr);
  }
}
