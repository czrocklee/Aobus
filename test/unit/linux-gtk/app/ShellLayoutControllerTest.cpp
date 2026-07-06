// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutController.h"

#include "app/AppConfig.h"
#include "app/GtkUiServices.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/ShellLayoutStore.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Transport.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/PlaybackSessionState.h>
#include <ao/uimodel/layout/action/LayoutActionTypes.h>
#include <ao/uimodel/layout/component/ILayoutComponentStateStore.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/applicationwindow.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <tuple>

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

  TEST_CASE("ShellLayoutController attaches layout shell and persists panel state", "[gtk][unit][app][shell]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::ApplicationWindow{};
    window.set_application(appPtr);

    auto const tempDir = fixture.tempDir().path();
    auto const configPtr = std::make_shared<AppConfig>(tempDir / "config.yaml");
    auto const storePtr = std::make_shared<ShellLayoutStore>(tempDir / "layouts");
    auto const componentStateStorePtr = std::make_shared<ShellLayoutComponentStateStore>(tempDir / "layout-state");
    auto themeController = ThemeCoordinator{};
    auto queueModel = uimodel::PlaybackQueueModel{runtime.playback(), runtime.notifications()};
    auto commandSurface = uimodel::PlaybackCommandSurface{
      runtime.playback(), &queueModel, [&runtime] { std::ignore = runtime.playSelectionInFocusedView(); }};
    auto controller =
      ShellLayoutController{runtime, window, configPtr, storePtr, componentStateStorePtr, themeController};
    controller.bindServices(
      GtkUiServices{.playbackQueueModel = &queueModel, .playbackCommandSurface = &commandSurface});

    SECTION("attachToWindow sets child")
    {
      controller.attachToWindow();
      CHECK(window.get_child() != nullptr);
    }

    SECTION("loadLayout load works")
    {
      controller.loadLayout(*configPtr);
      drainGtkEvents();
      CHECK(controller.context().componentStateStore ==
            static_cast<uimodel::ILayoutComponentStateStore*>(componentStateStorePtr.get()));
    }

    SECTION("layout editor cancel rolls back theme preview without changing persisted theme")
    {
      auto prefs = rt::AppPrefsState{};
      prefs.lastLayoutPreset = "classic";
      prefs.lastThemePreset = "classic";
      configPtr->saveAppPrefs(prefs);
      themeController.load(*configPtr);

      controller.loadLayout(*configPtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));

      controller.openEditor(*configPtr);
      drainGtkEvents();

      auto* const dialog = controller.editorDialogForTest();
      REQUIRE(dialog != nullptr);
      CHECK(dialog->selectedThemeId() == "classic");

      dialog->setSelectedThemeIdForTest("modern");
      drainGtkEvents();
      CHECK(themeController.activeTheme() == rt::ThemePresetId::Modern);

      auto* const cancelButton = findButtonByLabel(*dialog, "Cancel");
      REQUIRE(cancelButton != nullptr);
      emitClicked(*cancelButton);
      drainGtkEvents();

      auto savedPrefs = rt::AppPrefsState{};
      configPtr->loadAppPrefs(savedPrefs);
      CHECK(savedPrefs.lastThemePreset == "classic");
      CHECK(themeController.activeTheme() == rt::ThemePresetId::Classic);
    }

    SECTION("layout editor save does not persist the previewed theme")
    {
      auto prefs = rt::AppPrefsState{};
      prefs.lastLayoutPreset = "classic";
      prefs.lastThemePreset = "classic";
      configPtr->saveAppPrefs(prefs);
      themeController.load(*configPtr);

      controller.loadLayout(*configPtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));

      controller.openEditor(*configPtr);
      drainGtkEvents();

      auto* const dialog = controller.editorDialogForTest();
      REQUIRE(dialog != nullptr);

      dialog->setSelectedThemeIdForTest("modern");
      drainGtkEvents();
      CHECK(themeController.activeTheme() == rt::ThemePresetId::Modern);

      auto* const saveButton = findButtonByLabel(*dialog, "Save");
      REQUIRE(saveButton != nullptr);
      emitClicked(*saveButton);
      drainGtkEvents();

      auto savedPrefs = rt::AppPrefsState{};
      configPtr->loadAppPrefs(savedPrefs);
      CHECK(savedPrefs.lastLayoutPreset == "classic");
      CHECK(savedPrefs.lastThemePreset == "classic");
      CHECK(themeController.activeTheme() == rt::ThemePresetId::Classic);
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
      REQUIRE(runtime.playback().restoreSession(rt::PlaybackSessionState{
        .sourceListId = ListId{5},
        .trackId = trackId,
        .positionMs = 50,
      }));
      controller.attachToWindow();
      controller.refreshExportedActions();

      auto* actionMap = dynamic_cast<Gio::ActionMap*>(&window);
      REQUIRE(actionMap != nullptr);
      auto const stopActionPtr = actionMap->lookup_action("playback.stop");
      REQUIRE(stopActionPtr != nullptr);
      CHECK(stopActionPtr->property_enabled() == false);

      auto const playPauseOutcome = controller.activateAction("playback.playPause");
      CHECK(playPauseOutcome.result == uimodel::LayoutActionActivationResult::Activated);
      CHECK(runtime.playback().state().transport == audio::Transport::Playing);
      CHECK(runtime.playback().state().trackId == trackId);

      controller.refreshExportedActions();
      CHECK(stopActionPtr->property_enabled() == true);

      auto const stopOutcome = controller.activateAction("playback.stop");
      CHECK(stopOutcome.result == uimodel::LayoutActionActivationResult::Activated);
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

      controller.loadLayout(*configPtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));
      REQUIRE(controller.context().componentState.components.contains("main-paned"));

      controller.resetRuntimeLayoutState();

      CHECK(controller.context().componentState.components.empty());
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

      controller.loadLayout(*configPtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));

      controller.setConfirmPromotionCallback(
        [](std::string const& /*presetId*/, ShellLayoutController::ConfirmPromotionAnswer answer) { answer(false); });
      controller.saveCurrentPanelSizesAsLayoutDefaults();

      auto optSavedDoc = storePtr->load("classic");
      REQUIRE(optSavedDoc.has_value());

      auto const* savedSplit = findNodeById(optSavedDoc->root, "main-paned");
      REQUIRE(savedSplit != nullptr);
      CHECK(savedSplit->props.at("position").asInt() == 200);

      auto optUntouchedState = componentStateStorePtr->load("classic");
      REQUIRE(optUntouchedState.has_value());
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

      controller.loadLayout(*configPtr);
      REQUIRE(pumpGtkEventsUntil([&controller]
                                 { return findNodeById(controller.activeLayout().root, "main-paned") != nullptr; }));

      controller.setConfirmPromotionCallback(
        [](std::string const& /*presetId*/, ShellLayoutController::ConfirmPromotionAnswer answer) { answer(true); });
      controller.saveCurrentPanelSizesAsLayoutDefaults();

      auto optSavedDoc = storePtr->load("classic");
      REQUIRE(optSavedDoc.has_value());

      auto const* savedSplit = findNodeById(optSavedDoc->root, "main-paned");
      auto const* savedCollapsible = findNodeById(optSavedDoc->root, "detail-split");
      REQUIRE(savedSplit != nullptr);
      REQUIRE(savedCollapsible != nullptr);

      CHECK(savedSplit->props.find("position") == savedSplit->props.end());
      CHECK(savedSplit->props.at("initialPositionPercent").asDouble() == 0.42);
      CHECK(savedCollapsible->props.at("position").asInt() == 320);
      CHECK(savedCollapsible->props.find("initialPositionPercent") == savedCollapsible->props.end());

      auto optPromotedState = componentStateStorePtr->load("classic");
      REQUIRE(optPromotedState.has_value());
      CHECK_FALSE(optPromotedState->components.contains("main-paned"));
      REQUIRE(optPromotedState->components.contains("detail-split"));
      auto const& remainingEntry = optPromotedState->components.at("detail-split");
      CHECK(remainingEntry.baselineHash == uimodel::componentBaselineHash(*savedCollapsible));
      CHECK(remainingEntry.state.size() == 1);
      CHECK(remainingEntry.state.at("revealed").asBool(true) == false);
    }
  }
} // namespace ao::gtk::test
