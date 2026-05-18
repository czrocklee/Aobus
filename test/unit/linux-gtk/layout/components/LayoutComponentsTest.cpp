// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <app/linux-gtk/layout/document/LayoutDocument.h>
#include <app/linux-gtk/layout/document/LayoutNode.h>
#include <app/linux-gtk/layout/document/LayoutYaml.h> // NOLINT(misc-include-cleaner)
#include <app/linux-gtk/layout/runtime/ComponentRegistry.h>
#include <app/linux-gtk/layout/runtime/LayoutRuntime.h>

#include <app/linux-gtk/inspector/CoverArtCache.h>
#include <app/linux-gtk/track/TrackRowCache.h>
#include <app/runtime/AppRuntime.h>
#include <app/runtime/ConfigStore.h>
#include <runtime/CorePrimitives.h>

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

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace ao::lmdb::test;

  namespace
  {
    class MockExecutor final : public rt::IControlExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };

    LayoutContext makeContext(ComponentRegistry& registry, rt::AppRuntime& runtime, Gtk::Window& window)
    {
      return LayoutContext{.registry = registry, .runtime = runtime, .parentWindow = window};
    }
  } // namespace

  TEST_CASE("Playback component instantiation", "[layout][components]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto const executor = std::make_shared<MockExecutor>();
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto ctx = makeContext(registry, runtime, window);

    SECTION("playPauseButton creates Gtk::Button")
    {
      auto const node = LayoutNode{.type = "playback.playPauseButton"};
      auto const comp = registry.create(ctx, node);

      REQUIRE(comp != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&comp->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-start-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
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
      CHECK(btn->has_css_class("ao-playback-button"));
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
      CHECK(btn->has_css_class("ao-playback-button"));
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
      CHECK(btn->has_css_class("ao-playback-button"));
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
      CHECK(scale->has_css_class("ao-seekbar"));
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
      CHECK(label->has_css_class("ao-playback-title"));
    }

    SECTION("currentArtistLabel shows empty when idle")
    {
      auto const node = LayoutNode{.type = "playback.currentArtistLabel"};
      auto const comp = registry.create(ctx, node);

      REQUIRE(comp != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text().empty());
      CHECK(label->has_css_class("ao-playback-artist"));
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
      CHECK(btn->has_css_class("ao-output-logo"));

      // CSS controls the hit-target size; no explicit set_size_request.
      int buttonWidth = -1;
      int buttonHeight = -1;
      btn->get_size_request(buttonWidth, buttonHeight);
      CHECK(buttonWidth == -1);
      CHECK(buttonHeight == -1);

      auto* const child = btn->get_child();
      REQUIRE(child != nullptr);
      CHECK(child->has_css_class("ao-soul"));

      // CSS controls the glyph size; no explicit set_size_request.
      int childWidth = -1;
      int childHeight = -1;
      child->get_size_request(childWidth, childHeight);
      CHECK(childWidth == -1);
      CHECK(childHeight == -1);
    }

    SECTION("qualityIndicator creates AobusSoul widget")
    {
      auto const node = LayoutNode{.type = "playback.qualityIndicator"};
      auto const comp = registry.create(ctx, node);

      REQUIRE(comp != nullptr);
      CHECK(comp->widget().has_css_class("ao-soul"));

      // CSS controls the glyph size; no explicit set_size_request.
      int width = -1;
      int height = -1;
      comp->widget().get_size_request(width, height);
      CHECK(width == -1);
      CHECK(height == -1);
    }

    SECTION("all 11 playback types register and instantiate")
    {
      auto const types = std::to_array<std::string_view>({"playback.playPauseButton",
                                                          "playback.stopButton",
                                                          "playback.volumeControl",
                                                          "playback.currentTitleLabel",
                                                          "playback.currentArtistLabel",
                                                          "playback.seekSlider",
                                                          "playback.timeLabel",
                                                          "playback.playButton",
                                                          "playback.pauseButton",
                                                          "playback.outputButton",
                                                          "playback.qualityIndicator"});

      for (auto const type : types)
      {
        auto const node = LayoutNode{.type = std::string{type}};
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
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto ctx = makeContext(registry, runtime, window);

    SECTION("library.listTree shows error when rowDataProvider missing")
    {
      auto const node = LayoutNode{.type = "library.listTree"};
      auto const comp = registry.create(ctx, node);

      REQUIRE(comp != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("trackRowCache missing") != std::string::npos);
    }

    SECTION("library.listTree shows error when listSidebarController missing")
    {
      auto const rdp = std::make_unique<TrackRowCache>(runtime.musicLibrary());
      ctx.track.trackRowCache = rdp.get();
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
      CHECK(label->get_label().find("trackPageHost missing") != std::string::npos);
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
      CHECK(label->get_label().find("trackPageHost missing") != std::string::npos);
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
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};

    int const cacheSize = 10;
    auto coverArtCache = std::make_unique<CoverArtCache>(cacheSize);
    auto menuModel = Gio::Menu::create();
    menuModel->append("Test Item", "win.test");

    auto ctx = LayoutContext{.registry = registry,
                             .runtime = runtime,
                             .parentWindow = window,
                             .inspector = {.coverArtCache = coverArtCache.get()},
                             .shell = {.menuModel = menuModel}};

    [[maybe_unused]] auto layoutRuntime = LayoutRuntime{registry};

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
      auto ctx2 = makeContext(registry, runtime, window);
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

    SECTION("inspector.sidebar creates TrackInspectorPanel when cache available")
    {
      auto const node = LayoutNode{.type = "inspector.sidebar"};
      auto const comp = registry.create(ctx, node);

      REQUIRE(comp != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&comp->widget());
      CHECK(label == nullptr);
      CHECK(comp->widget().has_css_class("ao-inspector-sidebar"));
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
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto ctx = makeContext(registry, runtime, window);

    SECTION("all 13 status and semantic types")
    {
      auto const types = std::to_array<std::string_view>({"status.messageLabel",
                                                          "library.listTree",
                                                          "tracks.table",
                                                          "library.openLibraryButton",
                                                          "inspector.coverArt",
                                                          "inspector.sidebar",
                                                          "app.menuBar",
                                                          "app.workspaceWithInspector",
                                                          "status.playbackDetails",
                                                          "status.nowPlaying",
                                                          "status.importProgress",
                                                          "status.notification",
                                                          "status.trackCount"});

      for (auto const type : types)
      {
        auto const node = LayoutNode{.type = std::string{type}};
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
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto ctx = makeContext(registry, runtime, window);

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
      auto const* const yaml = R"(
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

      auto layoutRuntime = LayoutRuntime{registry};
      auto const comp = layoutRuntime.build(ctx, doc);

      REQUIRE(comp != nullptr);
    }
  }
} // namespace ao::gtk::layout::test
