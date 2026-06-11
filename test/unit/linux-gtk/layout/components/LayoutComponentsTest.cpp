// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/layout/document/LayoutNode.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "app/linux-gtk/track/TrackRowCache.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/LayoutYaml.h>
#include <ao/yaml/Utils.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popovermenubar.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace ao::lmdb::test;
  using ao::gtk::test::makeRuntime;

  namespace
  {
    struct WidgetMeasure final
    {
      std::int32_t minimum = 0;
      std::int32_t natural = 0;
      std::int32_t minimumBaseline = -1;
      std::int32_t naturalBaseline = -1;
    };

    LayoutContext makeContext(ComponentRegistry& registry,
                              ActionRegistry& actionRegistry,
                              rt::AppRuntime& runtime,
                              Gtk::Window& window)
    {
      return LayoutContext{
        .registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};
    }

    WidgetMeasure measureWidget(Gtk::Widget& widget, Gtk::Orientation orientation, std::int32_t forSize)
    {
      auto result = WidgetMeasure{};
      widget.measure(
        orientation, forSize, result.minimum, result.natural, result.minimumBaseline, result.naturalBaseline);
      return result;
    }

    void walkWidgets(Gtk::Widget& root, auto const& visit)
    {
      visit(root);

      for (auto* child = root.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        walkWidgets(*child, visit);
      }
    }

    template<typename WidgetT>
    WidgetT* findWidget(Gtk::Widget& root)
    {
      WidgetT* result = nullptr;

      walkWidgets(root,
                  [&](Gtk::Widget& widget)
                  {
                    if (result == nullptr)
                    {
                      result = dynamic_cast<WidgetT*>(&widget);
                    }
                  });

      return result;
    }
  } // namespace

  TEST_CASE("Playback component instantiation", "[layout][unit][components]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto actionRegistry = ActionRegistry{};
    auto ctx = makeContext(registry, actionRegistry, runtime, window);

    SECTION("playPauseButton creates Gtk::Button")
    {
      auto const node = LayoutNode{.type = "playback.playPauseButton"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-start-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("stopButton creates Gtk::Button, insensitive when idle")
    {
      auto const node = LayoutNode{.type = "playback.stopButton"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-stop-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("playButton creates Gtk::Button, insensitive when not ready")
    {
      auto const node = LayoutNode{.type = "playback.playButton"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-start-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("pauseButton creates Gtk::Button, insensitive when not playing")
    {
      auto const node = LayoutNode{.type = "playback.pauseButton"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-pause-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("seekSlider creates Gtk::Scale, insensitive when idle")
    {
      auto const node = LayoutNode{.type = "playback.seekSlider"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const scale = dynamic_cast<Gtk::Scale*>(&compPtr->widget());
      REQUIRE(scale != nullptr);
      CHECK(scale->get_sensitive() == false);
      CHECK(scale->get_value() == 0.0);
      CHECK(scale->has_css_class("ao-seekbar"));
    }

    SECTION("timeLabel creates Gtk::Label with default text")
    {
      auto const node = LayoutNode{.type = "playback.timeLabel"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "00:00 / 00:00");
    }

    SECTION("currentTitleLabel shows Not Playing when idle")
    {
      auto const node = LayoutNode{.type = "playback.currentTitleLabel"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Not Playing");
      CHECK(label->has_css_class("ao-playback-title"));
    }

    SECTION("currentArtistLabel shows empty when idle")
    {
      auto const node = LayoutNode{.type = "playback.currentArtistLabel"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text().empty());
      CHECK(label->has_css_class("ao-playback-artist"));
    }

    SECTION("volumeControl shows hidden when volume unavailable")
    {
      auto const node = LayoutNode{.type = "playback.volumeControl"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);
      CHECK(compPtr->widget().get_visible() == false);
    }

    SECTION("qualityIndicator creates AobusSoul widget")
    {
      auto const node = LayoutNode{.type = "playback.qualityIndicator"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      CHECK(widget.has_css_class("ao-soul"));

      // CSS controls the glyph size; no explicit set_size_request.
      std::int32_t widgetWidth = -1;
      std::int32_t widgetHeight = -1;
      widget.get_size_request(widgetWidth, widgetHeight);
      CHECK(widgetWidth == -1);
      CHECK(widgetHeight == -1);
    }

    SECTION("soulButton creates Gtk::Button with AobusSoul")
    {
      auto const node = LayoutNode{.type = "playback.soulButton"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      CHECK(button->get_has_frame() == false);
      CHECK(button->has_css_class("ao-soul-button"));

      auto* const soul = button->get_child();
      REQUIRE(soul != nullptr);
      CHECK(soul->has_css_class("ao-soul"));

      std::int32_t soulWidth = -1;
      std::int32_t soulHeight = -1;
      soul->get_size_request(soulWidth, soulHeight);
      CHECK(soulWidth == -1);
      CHECK(soulHeight == -1);

      CHECK(soul->get_hexpand() == false);
      CHECK(soul->get_vexpand() == false);
      CHECK(soul->get_halign() == Gtk::Align::FILL);
      CHECK(soul->get_valign() == Gtk::Align::FILL);
    }

    SECTION("outputSelector creates Gtk::Button with Label")
    {
      auto const node = LayoutNode{.type = "playback.outputSelector"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      CHECK(button->get_has_frame() == false);
      CHECK(button->has_css_class("ao-output-selector-modern"));

      auto* const label = dynamic_cast<Gtk::Label*>(button->get_child());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "--"); // Default backend summary
    }

    SECTION("all 13 playback types register and instantiate")
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
                                                          "playback.qualityIndicator",
                                                          "playback.soulPlayPauseButton",
                                                          "playback.soulButton",
                                                          "playback.outputSelector"});

      for (auto const type : types)
      {
        auto const node = LayoutNode{.type = std::string{type}};
        auto const compPtr = registry.create(ctx, node);
        CHECK(compPtr != nullptr);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Semantic components — error states
  // ---------------------------------------------------------------------------
  TEST_CASE("Semantic component error states", "[layout][unit][components]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto actionRegistry = ActionRegistry{};
    auto ctx = makeContext(registry, actionRegistry, runtime, window);

    SECTION("library.listTree shows error when rowDataProvider missing")
    {
      auto const node = LayoutNode{.type = "library.listTree"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("trackRowCache missing") != std::string::npos);
    }

    SECTION("library.listTree shows error when listNavigationController missing")
    {
      auto const rdpPtr = std::make_unique<TrackRowCache>(runtime.musicLibrary());
      ctx.track.trackRowCache = rdpPtr.get();
      auto const node = LayoutNode{.type = "library.listTree"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("listNavigationController missing") != std::string::npos);
    }

    SECTION("tracks.table shows error when trackPageGraph missing")
    {
      auto const node = LayoutNode{.type = "tracks.table"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
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
  TEST_CASE("Semantic component success states", "[layout][unit][components]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};

    int const cacheSize = 10;
    auto imageCachePtr = std::make_unique<ImageCache>(cacheSize);
    auto menuModelPtr = Gio::Menu::create();
    menuModelPtr->append_submenu("Test Menu", Gio::Menu::create());

    auto actionRegistry = ActionRegistry{};
    auto ctx = LayoutContext{.registry = registry,
                             .actionRegistry = actionRegistry,
                             .runtime = runtime,
                             .parentWindow = window,
                             .detail = {.imageCache = imageCachePtr.get()},
                             .shell = {.menuModelPtr = menuModelPtr}};

    [[maybe_unused]] auto layoutRuntime = LayoutRuntime{registry};

    {
      auto const node = LayoutNode{.type = "status.messageLabel"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Aobus Ready");
    }

    SECTION("library.openLibraryButton creates Gtk::Button")
    {
      auto const node = LayoutNode{.type = "library.openLibraryButton"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "folder-open-symbolic");
    }

    SECTION("app.menuBar creates Gtk::PopoverMenuBar")
    {
      auto const node = LayoutNode{.type = "app.menuBar"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const menuBar = dynamic_cast<Gtk::PopoverMenuBar*>(&compPtr->widget());
      REQUIRE(menuBar != nullptr);
    }

    SECTION("app.menuButton creates Gtk::MenuButton and sets menu model")
    {
      auto const node = LayoutNode{.type = "app.menuButton", .props = {{"icon", LayoutValue{"test-icon"}}}};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto* const menuButton = dynamic_cast<Gtk::MenuButton*>(&compPtr->widget());
      REQUIRE(menuButton != nullptr);
      CHECK(menuButton->get_icon_name() == "test-icon");
      CHECK(menuButton->get_menu_model() == menuModelPtr);
    }

    SECTION("app.menuBar tolerates absent menu model")
    {
      auto actionRegistry2 = ActionRegistry{};
      auto ctx2 = makeContext(registry, actionRegistry2, runtime, window);
      auto const node = LayoutNode{.type = "app.menuBar"};
      auto const compPtr = registry.create(ctx2, node);

      REQUIRE(compPtr != nullptr);
      CHECK(dynamic_cast<Gtk::PopoverMenuBar*>(&compPtr->widget()) != nullptr);
    }

    SECTION("track.detailScope creates box and acts as scope provider")
    {
      auto const node = LayoutNode{.type = "track.detailScope"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);
      CHECK(dynamic_cast<Gtk::Box*>(&compPtr->widget()) != nullptr);
      CHECK(ctx.track.detailScope == nullptr); // Ensure context is restored
    }

    SECTION("track.selectionRegion creates box container")
    {
      auto const node = LayoutNode{.type = "track.selectionRegion"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);
      CHECK(dynamic_cast<Gtk::Box*>(&compPtr->widget()) != nullptr);
    }

    SECTION("track.coverArt creates a stable responsive square slot")
    {
      auto node = LayoutNode{.type = "track.coverArt"};
      node.props["targetSize"] = LayoutValue{static_cast<std::int64_t>(250)};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      CHECK(widget.get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK(widget.get_first_child() != nullptr);

      auto const horizontalMeasure = measureWidget(widget, Gtk::Orientation::HORIZONTAL, -1);
      CHECK(horizontalMeasure.minimum == 0);
      CHECK(horizontalMeasure.natural == 250);

      auto const heightConstrainedHorizontalMeasure = measureWidget(widget, Gtk::Orientation::HORIZONTAL, 233);
      CHECK(heightConstrainedHorizontalMeasure.minimum == 0);
      CHECK(heightConstrainedHorizontalMeasure.natural == 233);

      auto const unconstrainedVerticalMeasure = measureWidget(widget, Gtk::Orientation::VERTICAL, -1);
      CHECK(unconstrainedVerticalMeasure.minimum == 0);
      CHECK(unconstrainedVerticalMeasure.natural == 250);

      auto const narrowVerticalMeasure = measureWidget(widget, Gtk::Orientation::VERTICAL, 180);
      CHECK(narrowVerticalMeasure.minimum == 0);
      CHECK(narrowVerticalMeasure.natural == 180);

      auto const wideVerticalMeasure = measureWidget(widget, Gtk::Orientation::VERTICAL, 320);
      CHECK(wideVerticalMeasure.minimum == 0);
      CHECK(wideVerticalMeasure.natural == 250);

      auto* const imageWidget = widget.get_first_child();
      REQUIRE(imageWidget != nullptr);
      window.set_child(widget);
      widget.set_visible(true);
      imageWidget->set_visible(true);

      widget.size_allocate(Gtk::Allocation{0, 0, 180, 300}, -1);
      CHECK(widget.get_width() == 180);

      window.unset_child();
    }

    SECTION("track.fieldGrid creates grid and acts as scope subscriber")
    {
      auto const node = LayoutNode{.type = "track.fieldGrid"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);
      // Returns a main box containing: [fixed viewport [scroll [constrained wrapper [Grid]]], undo bar]
      auto& root = compPtr->widget();
      auto* const scrolled = findWidget<Gtk::ScrolledWindow>(root);
      auto* const grid = findWidget<Gtk::Grid>(root);
      CHECK(scrolled != nullptr);
      CHECK(grid != nullptr);
    }

    SECTION("track.tagEditor creates tag editor container")
    {
      auto const node = LayoutNode{.type = "track.tagEditor"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);
      CHECK(!compPtr->widget().get_name().empty());
    }
  }

  // ---------------------------------------------------------------------------
  // All types registration
  // ---------------------------------------------------------------------------
  TEST_CASE("All component types register and instantiate", "[layout][unit][components]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto actionRegistry = ActionRegistry{};
    auto ctx = makeContext(registry, actionRegistry, runtime, window);

    SECTION("all 20 status and semantic types")
    {
      auto const types = std::to_array<std::string_view>({"status.messageLabel",
                                                          "library.listTree",
                                                          "tracks.table",
                                                          "library.openLibraryButton",
                                                          "app.menuBar",
                                                          "status.playbackDetails",
                                                          "status.nowPlaying",
                                                          "status.importProgress",
                                                          "status.notification",
                                                          "status.trackCount",
                                                          "track.detailScope",
                                                          "track.selectionRegion",
                                                          "track.coverArt",
                                                          "track.fieldGrid",
                                                          "track.tagEditor",
                                                          "track.quickFilter"});

      for (auto const type : types)
      {
        auto const node = LayoutNode{.type = std::string{type}};
        auto const compPtr = registry.create(ctx, node);
        CHECK(compPtr != nullptr);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // YAML round-trip with semantic components
  // ---------------------------------------------------------------------------
  TEST_CASE("YAML layout with semantic components", "[layout][unit][components]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto actionRegistry = ActionRegistry{};
    auto ctx = makeContext(registry, actionRegistry, runtime, window);

    SECTION("app.actionButton builds from YAML and binds actions")
    {
      auto const* const yaml = R"(
      type: app.actionButton
      props:
        label: "Settings"
        icon: "emblem-system-symbolic"
        style: "circular"
        primaryAction: "shell.showSystemMenu"
        primaryLongPressAction: "shell.showSoul"
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
      auto layoutNode = LayoutNode{};
      REQUIRE(yaml::read(tree.rootref(), layoutNode));

      std::int32_t primaryFired = 0;
      std::int32_t longPressFired = 0;

      actionRegistry.registerAction(ActionDescriptor{.id = "shell.showSystemMenu",
                                                     .label = "System Menu",
                                                     .category = "Shell",
                                                     .capabilities = ActionCapability::None},
                                    [&](ActionActivationContext&) { primaryFired++; });

      actionRegistry.registerAction(
        ActionDescriptor{
          .id = "shell.showSoul", .label = "Show Soul", .category = "Shell", .capabilities = ActionCapability::None},
        [&](ActionActivationContext&) { longPressFired++; });

      auto const compPtr = registry.create(ctx, layoutNode);
      REQUIRE(compPtr != nullptr);

      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      CHECK(button->get_icon_name() == "emblem-system-symbolic");
      CHECK(button->has_css_class("circular"));

      // Verify that clicking the button routes primary action through the registry
      ::g_signal_emit_by_name(button->gobj(), "clicked");
      CHECK(primaryFired == 1);
      CHECK(longPressFired == 0);
    }

    SECTION("app.actionButton exposes enum properties for editor")
    {
      auto const optDesc = registry.descriptor("app.actionButton");
      REQUIRE(optDesc.has_value());

      auto const it = std::find_if(
        optDesc->props.begin(), optDesc->props.end(), [](auto const& p) { return p.name == "primaryAction"; });
      REQUIRE(it != optDesc->props.end());
      CHECK(it->kind == PropertyKind::Enum);
      CHECK(it->enumValues.empty());
      REQUIRE(it->optActionBinding.has_value());
      CHECK(it->optActionBinding.value().slot == ActionSlot::PrimaryClick);
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
        - type: playback.playPauseButton
        - type: playback.stopButton
        - type: playback.seekSlider
          layout:
            hexpand: true
        - type: playback.timeLabel
        - type: playback.volumeControl
    )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
      auto layoutNode = LayoutNode{};
      REQUIRE(yaml::read(tree.rootref(), layoutNode));

      auto const compPtr = registry.create(ctx, layoutNode);
      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);

      auto* const child = box->get_first_child();
      REQUIRE(child != nullptr);
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
            - type: playback.playPauseButton
            - type: playback.stopButton
            - type: playback.volumeControl
    )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
      auto layoutNode = LayoutNode{};
      REQUIRE(yaml::read(tree.rootref(), layoutNode));

      auto const compPtr = registry.create(ctx, layoutNode);
      REQUIRE(compPtr != nullptr);

      auto* const outerBox = dynamic_cast<Gtk::Box*>(&compPtr->widget());
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

      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);
      auto doc = LayoutDocument{};
      REQUIRE(yaml::read(tree.rootref(), doc));

      CHECK(doc.version == 1);
      CHECK(doc.root.children.size() == 4);

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);
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
      auto doc = LayoutDocument{};
      doc.templates = getBuiltInTemplates();
      REQUIRE(yaml::read(tree.rootref(), doc));

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);
    }
  }
} // namespace ao::gtk::layout::test
