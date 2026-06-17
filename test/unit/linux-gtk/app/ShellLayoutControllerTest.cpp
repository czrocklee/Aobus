// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutController.h"

#include "app/AppConfig.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/ShellLayoutStore.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/layout/ILayoutComponentStateStore.h>
#include <ao/uimodel/layout/LayoutComponentState.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/applicationwindow.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>

namespace ao::gtk::test
{
  using namespace uimodel::layout;
  namespace
  {
    uimodel::layout::LayoutNode splitNode(std::string_view id)
    {
      auto node = uimodel::layout::LayoutNode{};
      node.id = std::string{id};
      node.type = "split";
      node.props["orientation"] = uimodel::layout::LayoutValue{std::string{"horizontal"}};
      node.props["position"] = uimodel::layout::LayoutValue{static_cast<std::int64_t>(200)};
      node.children.push_back(uimodel::layout::LayoutNode{.type = "spacer"});
      node.children.push_back(uimodel::layout::LayoutNode{.type = "spacer"});
      return node;
    }

    uimodel::layout::LayoutNode collapsibleSplitNode(std::string_view id)
    {
      auto node = uimodel::layout::LayoutNode{};
      node.id = std::string{id};
      node.type = "collapsibleSplit";
      node.props["orientation"] = uimodel::layout::LayoutValue{std::string{"horizontal"}};
      node.props["position"] = uimodel::layout::LayoutValue{static_cast<std::int64_t>(150)};
      node.props["initialPositionPercent"] = uimodel::layout::LayoutValue{0.25};
      node.props["revealed"] = uimodel::layout::LayoutValue{true};
      node.children.push_back(uimodel::layout::LayoutNode{.type = "spacer"});
      node.children.push_back(uimodel::layout::LayoutNode{.type = "spacer"});
      return node;
    }

    uimodel::layout::LayoutDocument panelLayoutDocument()
    {
      auto doc = uimodel::layout::LayoutDocument{};
      doc.root.type = "box";
      doc.root.props["orientation"] = uimodel::layout::LayoutValue{std::string{"vertical"}};
      doc.root.children.push_back(splitNode("main-paned"));
      doc.root.children.push_back(collapsibleSplitNode("detail-split"));
      return doc;
    }

    uimodel::layout::LayoutNode const* findNodeById(uimodel::layout::LayoutNode const& node, std::string_view id)
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

    template<typename Predicate>
    bool drainGtkEventsUntil(Predicate const& predicate)
    {
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};

      while (std::chrono::steady_clock::now() < deadline)
      {
        drainGtkEvents();

        if (predicate())
        {
          return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }

      drainGtkEvents();
      return predicate();
    }
  } // namespace

  TEST_CASE("ShellLayoutController - lifecycle", "[gtk][app][shell]")
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
    auto controller =
      ShellLayoutController{runtime, window, configPtr, storePtr, componentStateStorePtr, themeController};

    SECTION("initial state")
    {
      // Registry should have standard components
      // We don't have public way to check count without peering
    }

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
            static_cast<uimodel::layout::ILayoutComponentStateStore*>(componentStateStorePtr.get()));
    }

    SECTION("attachToWindow exports actions and refreshExportedActions works")
    {
      controller.attachToWindow();

      auto* actionMap = dynamic_cast<Gio::ActionMap*>(&window);
      REQUIRE(actionMap != nullptr);

      auto gioActionPtr = actionMap->lookup_action("playback.stop");
      REQUIRE(gioActionPtr != nullptr);

      // Layout state actions are owned by the window menu, not exported as shell.* actions.
      CHECK(actionMap->lookup_action("shell.resetRuntimeLayoutState") == nullptr);
      CHECK(actionMap->lookup_action("shell.saveCurrentPanelSizesAsLayoutDefaults") == nullptr);

      // Queue model is not bound, so hasActiveQueue is false, thus stop should be disabled
      controller.refreshExportedActions();
      CHECK(gioActionPtr->property_enabled() == false);
    }

    SECTION("resetRuntimeLayoutState clears preset state without removing customized layout")
    {
      auto doc = panelLayoutDocument();
      storePtr->save(doc, "classic");

      auto stateDoc = uimodel::layout::LayoutComponentStateDocument{.preset = "classic"};
      auto const* split = findNodeById(doc.root, "main-paned");
      REQUIRE(split != nullptr);
      stateDoc.components["main-paned"] = uimodel::layout::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::layout::kLayoutComponentStateEntryVersion,
        .baselineHash = uimodel::layout::layoutComponentBaselineHash(*split),
        .state = {{"positionPercent", uimodel::layout::LayoutValue{0.42}}},
      };
      componentStateStorePtr->save("classic", stateDoc);

      controller.loadLayout(*configPtr);
      REQUIRE(drainGtkEventsUntil([&controller]
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

      auto stateDoc = uimodel::layout::LayoutComponentStateDocument{.preset = "classic"};
      stateDoc.components["main-paned"] = uimodel::layout::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::layout::kLayoutComponentStateEntryVersion,
        .baselineHash = uimodel::layout::layoutComponentBaselineHash(*split),
        .state = {{"positionPercent", uimodel::layout::LayoutValue{0.42}}},
      };
      componentStateStorePtr->save("classic", stateDoc);

      controller.loadLayout(*configPtr);
      REQUIRE(drainGtkEventsUntil([&controller]
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

      auto stateDoc = uimodel::layout::LayoutComponentStateDocument{.preset = "classic"};
      stateDoc.components["main-paned"] = uimodel::layout::LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = uimodel::layout::kLayoutComponentStateEntryVersion,
        .baselineHash = uimodel::layout::layoutComponentBaselineHash(*split),
        .state = {{"positionPercent", uimodel::layout::LayoutValue{0.42}}},
      };
      stateDoc.components["detail-split"] = uimodel::layout::LayoutComponentStateEntry{
        .type = "collapsibleSplit",
        .stateVersion = uimodel::layout::kLayoutComponentStateEntryVersion,
        .baselineHash = uimodel::layout::layoutComponentBaselineHash(*collapsible),
        .state = {{"size", uimodel::layout::LayoutValue{static_cast<std::int64_t>(320)}},
                  {"revealed", uimodel::layout::LayoutValue{false}}},
      };
      componentStateStorePtr->save("classic", stateDoc);

      controller.loadLayout(*configPtr);
      REQUIRE(drainGtkEventsUntil([&controller]
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
      CHECK(remainingEntry.baselineHash == uimodel::layout::layoutComponentBaselineHash(*savedCollapsible));
      CHECK(remainingEntry.state.size() == 1);
      CHECK(remainingEntry.state.at("revealed").asBool(true) == false);
    }
  }
} // namespace ao::gtk::test
