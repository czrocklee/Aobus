// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutController.h"

#include "app/AppConfigStore.h"
#include "app/GtkUiDependencies.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/ShellLayoutStore.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Transport.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/layout/action/LayoutActionActivation.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateStore.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/paned.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::gtk::test
{
  using namespace uimodel;
  namespace
  {
    uimodel::LayoutNode splitNode(std::string_view id)
    {
      auto node = uimodel::LayoutNode{};
      node.id = std::string{id};
      node.type = "split";
      node.props["orientation"] = uimodel::LayoutValue{std::string{"horizontal"}};
      node.props["position"] = uimodel::LayoutValue{static_cast<std::int64_t>(200)};
      node.children.push_back(uimodel::LayoutNode{.type = "spacer"});
      node.children.push_back(uimodel::LayoutNode{.type = "spacer"});
      return node;
    }

    uimodel::LayoutNode collapsibleSplitNode(std::string_view id)
    {
      auto node = uimodel::LayoutNode{};
      node.id = std::string{id};
      node.type = "collapsibleSplit";
      node.props["orientation"] = uimodel::LayoutValue{std::string{"horizontal"}};
      node.props["position"] = uimodel::LayoutValue{static_cast<std::int64_t>(150)};
      node.props["initialPositionPercent"] = uimodel::LayoutValue{0.25};
      node.props["revealed"] = uimodel::LayoutValue{true};
      node.children.push_back(uimodel::LayoutNode{.type = "spacer"});
      node.children.push_back(uimodel::LayoutNode{.type = "spacer"});
      return node;
    }

    uimodel::LayoutDocument panelLayoutDocument()
    {
      auto doc = uimodel::LayoutDocument{};
      doc.root.type = "box";
      doc.root.props["orientation"] = uimodel::LayoutValue{std::string{"vertical"}};
      doc.root.children.push_back(splitNode("main-paned"));
      doc.root.children.push_back(collapsibleSplitNode("detail-split"));
      return doc;
    }

    uimodel::LayoutNode const* findNodeById(uimodel::LayoutNode const& node, std::string_view id)
    {
      if (node.id == id)
      {
        return &node;
      }

      for (auto const& child : node.children)
      {
        if (auto const* result = findNodeById(child, id); result != nullptr)
        {
          return result;
        }
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        return findNodeById(*node.optTooltip->nodePtr, id);
      }

      return nullptr;
    }
  } // namespace

  TEST_CASE("ShellLayoutController - attaches layout shell and persists panel state", "[gtk][unit][app][shell]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::ApplicationWindow{};
    window.set_application(appPtr);

    auto const tempDir = fixture.tempDir().path();
    auto const configStorePtr = std::make_shared<AppConfigStore>(tempDir / "config.yaml");
    auto const storePtr = std::make_shared<ShellLayoutStore>(tempDir / "layouts");
    auto const componentStateStorePtr = std::make_shared<ShellLayoutComponentStateStore>(tempDir / "layout-state");
    auto themeCoordinator = ThemeCoordinator{};
    auto commandSurface =
      uimodel::PlaybackCommandSurface{runtime.playback(),
                                      runtime.playbackSequence(),
                                      [&runtime] { std::ignore = runtime.playSelectionInFocusedView(); }};
    auto controller = ShellLayoutController{runtime,
                                            window,
                                            configStorePtr,
                                            storePtr,
                                            componentStateStorePtr,
                                            GtkUiDependencies{.playbackSequence = &runtime.playbackSequence(),
                                                              .playbackCommandSurface = &commandSurface,
                                                              .themeCoordinator = &themeCoordinator}};

    SECTION("attachToWindow sets child")
    {
      controller.attachToWindow();
      CHECK(window.get_child() != nullptr);
    }

    SECTION("loadLayout load works")
    {
      controller.loadLayout(*configStorePtr);
      drainGtkEvents();
      CHECK(controller.runtimeState().componentStateStore ==
            static_cast<uimodel::LayoutComponentStateStore*>(componentStateStorePtr.get()));
    }

    SECTION("layout editor cancel rolls back theme preview without changing persisted theme")
    {
      auto prefs = rt::AppPrefsState{};
      prefs.lastLayoutPreset = "classic";
      prefs.lastThemePreset = "classic";
      configStorePtr->saveAppPrefs(prefs);
      themeCoordinator.load(*configStorePtr);

      controller.loadLayout(*configStorePtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));

      controller.openEditor(*configStorePtr);
      drainGtkEvents();

      auto* const dialog = controller.editorDialog();
      REQUIRE(dialog != nullptr);
      CHECK(dialog->selectedThemeId() == "classic");

      dialog->setSelectedThemeId("modern");
      drainGtkEvents();
      CHECK(themeCoordinator.activeTheme() == rt::ThemePresetId::Modern);

      auto* const cancelButton = findButtonByLabel(*dialog, "Cancel");
      REQUIRE(cancelButton != nullptr);
      emitClicked(*cancelButton);
      drainGtkEvents();

      auto savedPrefs = rt::AppPrefsState{};
      configStorePtr->loadAppPrefs(savedPrefs);
      CHECK(savedPrefs.lastThemePreset == "classic");
      CHECK(themeCoordinator.activeTheme() == rt::ThemePresetId::Classic);
    }

    SECTION("layout editor save does not persist the previewed theme")
    {
      auto prefs = rt::AppPrefsState{};
      prefs.lastLayoutPreset = "classic";
      prefs.lastThemePreset = "classic";
      configStorePtr->saveAppPrefs(prefs);
      themeCoordinator.load(*configStorePtr);

      controller.loadLayout(*configStorePtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));

      controller.openEditor(*configStorePtr);
      drainGtkEvents();

      auto* const dialog = controller.editorDialog();
      REQUIRE(dialog != nullptr);

      dialog->setSelectedThemeId("modern");
      drainGtkEvents();
      CHECK(themeCoordinator.activeTheme() == rt::ThemePresetId::Modern);

      auto* const saveButton = findButtonByLabel(*dialog, "Save");
      REQUIRE(saveButton != nullptr);
      emitClicked(*saveButton);
      drainGtkEvents();

      auto savedPrefs = rt::AppPrefsState{};
      configStorePtr->loadAppPrefs(savedPrefs);
      CHECK(savedPrefs.lastLayoutPreset == "classic");
      CHECK(savedPrefs.lastThemePreset == "classic");
      CHECK(themeCoordinator.activeTheme() == rt::ThemePresetId::Classic);
    }

    SECTION("attachToWindow exports actions and refreshExportedActions works")
    {
      controller.attachToWindow();

      auto* actionMap = dynamic_cast<Gio::ActionMap*>(&window);
      REQUIRE(actionMap != nullptr);

      auto gioActionPtr = actionMap->lookup_action("playback.stop");
      REQUIRE(gioActionPtr != nullptr);
      CHECK(actionMap->lookup_action("playback.play") != nullptr);
      CHECK(actionMap->lookup_action("playback.pause") != nullptr);

      // Layout state actions are owned by the window menu, not exported as shell.* actions.
      CHECK(actionMap->lookup_action("shell.resetRuntimeLayoutState") == nullptr);
      CHECK(actionMap->lookup_action("shell.saveCurrentPanelSizesAsLayoutDefaults") == nullptr);

      // Nothing is playing yet, so stop should be disabled.
      controller.refreshExportedActions();
      CHECK(gioActionPtr->property_enabled() == false);
    }

    SECTION("playPause resumes restored idle now-playing and stop is transport-gated")
    {
      rt::test::addReadyAudioProvider(runtime.playback());
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      auto const trackId = library::test::addTrack(
        runtime.musicLibrary(), library::test::TrackSpec{.title = "Restored", .uri = fixturePath});
      runtime.reloadAllTracks();
      auto const view = runtime.workspace().navigateTo(rt::kAllTracksListId);
      REQUIRE(view);
      REQUIRE(runtime.playbackSequence().playFromView(*view, trackId));
      runtime.playback().seek(std::chrono::milliseconds{50});
      REQUIRE(runtime.savePlaybackSession());
      runtime.playback().stop();
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE(restored);
      REQUIRE(restored->restored);
      controller.attachToWindow();
      controller.refreshExportedActions();

      auto* actionMap = dynamic_cast<Gio::ActionMap*>(&window);
      REQUIRE(actionMap != nullptr);
      auto const stopActionPtr = actionMap->lookup_action("playback.stop");
      REQUIRE(stopActionPtr != nullptr);
      CHECK(stopActionPtr->property_enabled() == false);

      auto const playPauseResult = controller.activateAction("playback.playPause");
      CHECK(playPauseResult.outcome == uimodel::LayoutActionActivationOutcome::Activated);
      CHECK(runtime.playback().state().transport == audio::Transport::Playing);
      CHECK(runtime.playback().state().nowPlaying.trackId == trackId);

      controller.refreshExportedActions();
      CHECK(stopActionPtr->property_enabled() == true);

      auto const stopResult = controller.activateAction("playback.stop");
      CHECK(stopResult.outcome == uimodel::LayoutActionActivationOutcome::Activated);
      CHECK(runtime.playback().state().transport == audio::Transport::Idle);
    }

    SECTION("resetRuntimeLayoutState clears preset state without removing customized layout")
    {
      auto doc = panelLayoutDocument();
      storePtr->save(doc, "classic");

      auto stateDoc = uimodel::LayoutComponentStateDocument{.preset = "classic"};
      auto const* split = findNodeById(doc.root, "main-paned");
      REQUIRE(split != nullptr);
      stateDoc.components["main-paned"] = uimodel::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = uimodel::componentBaselineHash(*split),
        .state = {{"positionPercent", uimodel::LayoutValue{0.42}}},
      };
      componentStateStorePtr->save("classic", stateDoc);

      controller.loadLayout(*configStorePtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));
      REQUIRE(controller.runtimeState().componentState.components.contains("main-paned"));

      controller.resetRuntimeLayoutState();

      CHECK(controller.runtimeState().componentState.components.empty());
      CHECK_FALSE(componentStateStorePtr->load("classic").has_value());
      CHECK(storePtr->load("classic").has_value());
    }

    SECTION("saveCurrentPanelSizesAsLayoutDefaults cancels when the user declines")
    {
      auto doc = panelLayoutDocument();
      storePtr->save(doc, "classic");

      auto const* split = findNodeById(doc.root, "main-paned");
      REQUIRE(split != nullptr);

      auto stateDoc = uimodel::LayoutComponentStateDocument{.preset = "classic"};
      stateDoc.components["main-paned"] = uimodel::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = uimodel::componentBaselineHash(*split),
        .state = {{"positionPercent", uimodel::LayoutValue{0.42}}},
      };
      componentStateStorePtr->save("classic", stateDoc);

      controller.loadLayout(*configStorePtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));

      controller.setConfirmPromotionCallback(
        [](std::string const& /*presetId*/, ShellLayoutController::ConfirmPromotionAnswer answer) { answer(false); });
      controller.saveCurrentPanelSizesAsLayoutDefaults();

      auto optSavedDoc = storePtr->load("classic");
      REQUIRE(optSavedDoc);

      auto const* savedSplit = findNodeById(optSavedDoc->root, "main-paned");
      REQUIRE(savedSplit != nullptr);
      CHECK(savedSplit->props.at("position").asInt() == 200);

      auto optUntouchedState = componentStateStorePtr->load("classic");
      REQUIRE(optUntouchedState);
      CHECK(optUntouchedState->components.contains("main-paned"));
    }

    SECTION("saveCurrentPanelSizesAsLayoutDefaults promotes runtime panel state")
    {
      auto doc = panelLayoutDocument();
      storePtr->save(doc, "classic");

      auto const* split = findNodeById(doc.root, "main-paned");
      auto const* collapsible = findNodeById(doc.root, "detail-split");
      REQUIRE(split != nullptr);
      REQUIRE(collapsible != nullptr);

      auto stateDoc = uimodel::LayoutComponentStateDocument{.preset = "classic"};
      stateDoc.components["main-paned"] = uimodel::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = uimodel::componentBaselineHash(*split),
        .state = {{"positionPercent", uimodel::LayoutValue{0.42}}},
      };
      stateDoc.components["detail-split"] = uimodel::LayoutComponentStateEntry{
        .type = "collapsibleSplit",
        .stateVersion = uimodel::kStateEntryVersion,
        .baselineHash = uimodel::componentBaselineHash(*collapsible),
        .state = {{"size", uimodel::LayoutValue{static_cast<std::int64_t>(320)}},
                  {"revealed", uimodel::LayoutValue{false}}},
      };
      componentStateStorePtr->save("classic", stateDoc);

      controller.loadLayout(*configStorePtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));

      controller.setConfirmPromotionCallback(
        [](std::string const& /*presetId*/, ShellLayoutController::ConfirmPromotionAnswer answer) { answer(true); });
      controller.saveCurrentPanelSizesAsLayoutDefaults();

      auto optSavedDoc = storePtr->load("classic");
      REQUIRE(optSavedDoc);

      auto const* savedSplit = findNodeById(optSavedDoc->root, "main-paned");
      auto const* savedCollapsible = findNodeById(optSavedDoc->root, "detail-split");
      REQUIRE(savedSplit != nullptr);
      REQUIRE(savedCollapsible != nullptr);

      CHECK_FALSE(savedSplit->props.contains("position"));
      CHECK(savedSplit->props.at("initialPositionPercent").asDouble() == 0.42);
      CHECK(savedCollapsible->props.at("position").asInt() == 320);
      CHECK_FALSE(savedCollapsible->props.contains("initialPositionPercent"));

      auto optPromotedState = componentStateStorePtr->load("classic");
      REQUIRE(optPromotedState);
      CHECK_FALSE(optPromotedState->components.contains("main-paned"));
      REQUIRE(optPromotedState->components.contains("detail-split"));
      auto const& remainingEntry = optPromotedState->components.at("detail-split");
      CHECK(remainingEntry.baselineHash == uimodel::componentBaselineHash(*savedCollapsible));
      CHECK(remainingEntry.state.size() == 1);
      CHECK(remainingEntry.state.at("revealed").asBool(true) == false);
    }
  }

  TEST_CASE("ShellLayoutController - teardown flushes pending component state while its sole store owner is alive",
            "[gtk][regression][shell][lifecycle]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::ApplicationWindow{};
    window.set_application(appPtr);
    auto themeCoordinator = ThemeCoordinator{};
    auto commandSurface =
      uimodel::PlaybackCommandSurface{runtime.playback(),
                                      runtime.playbackSequence(),
                                      [&runtime] { std::ignore = runtime.playSelectionInFocusedView(); }};

    auto const tempDir = fixture.tempDir().path();
    auto const componentStateDir = tempDir / "layout-state";
    auto configStorePtr = std::make_shared<AppConfigStore>(tempDir / "config.yaml");
    auto* const configStore = configStorePtr.get();
    auto prefs = rt::AppPrefsState{};
    prefs.lastLayoutPreset = "classic";
    configStore->saveAppPrefs(prefs);
    auto layoutStorePtr = std::make_shared<ShellLayoutStore>(tempDir / "layouts");
    layoutStorePtr->save(panelLayoutDocument(), "classic");
    auto componentStateStorePtr = std::make_shared<ShellLayoutComponentStateStore>(componentStateDir);

    {
      auto controller = ShellLayoutController{runtime,
                                              window,
                                              std::move(configStorePtr),
                                              std::move(layoutStorePtr),
                                              std::move(componentStateStorePtr),
                                              GtkUiDependencies{.playbackSequence = &runtime.playbackSequence(),
                                                                .playbackCommandSurface = &commandSurface,
                                                                .themeCoordinator = &themeCoordinator}};
      controller.loadLayout(*configStore);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));

      auto allocationHost = AllocationHost{controller.host()};
      allocationHost.allocateChild(1000, 400);
      auto* const paned = findWidget<Gtk::Paned>(controller.host());
      REQUIRE(paned != nullptr);
      paned->set_position(400);
    }

    auto persistedStore = ShellLayoutComponentStateStore{componentStateDir};
    auto optState = persistedStore.load("classic");
    REQUIRE(optState);
    REQUIRE(optState->components.contains("main-paned"));
    CHECK(optState->components.at("main-paned").state.at("positionPercent").asDouble() == 0.4);
  }
} // namespace ao::gtk::test
