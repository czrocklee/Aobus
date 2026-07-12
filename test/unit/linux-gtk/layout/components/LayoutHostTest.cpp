// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/runtime/LayoutHost.h"

#include "app/linux-gtk/app/GtkUiDependencies.h"
#include "app/linux-gtk/layout/component/container/ContainerRegistry.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntimeState.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/layout/components/ContainerTestHelpers.h"
#include "test/unit/linux-gtk/layout/state/FakeLayoutComponentStateStore.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::makeRuntime;

  TEST_CASE("LayoutHost - rebuilds widget trees after layout updates", "[gtk][unit][layout][container]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    registerContainerComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto runtimeState = LayoutRuntimeState{};
    auto dependencies = GtkUiDependencies{};
    auto ctx = LayoutBuildContext{.registry = registry,
                                  .actionRegistry = actionRegistry,
                                  .runtime = runtime,
                                  .parentWindow = window,
                                  .runtimeState = runtimeState,
                                  .dependencies = dependencies};

    auto host = LayoutHost{registry};

    SECTION("Initial layout is empty before setLayout")
    {
      CHECK(host.get_first_child() == nullptr);
    }

    SECTION("setLayout with default document populates widget")
    {
      host.setLayout(ctx, makeDefaultLayout());

      auto* const child = host.get_first_child();

      REQUIRE(child != nullptr);
      CHECK(dynamic_cast<Gtk::Widget*>(child) != nullptr);
    }

    SECTION("setLayout replaces previous layout")
    {
      host.setLayout(ctx, makeDefaultLayout());

      auto* const first = host.get_first_child();
      CHECK(first != nullptr);

      auto newDoc = LayoutDocument{};
      newDoc.root.type = "spacer";
      host.setLayout(ctx, newDoc);

      auto* const second = host.get_first_child();
      CHECK(second != nullptr);
      CHECK(second != first);
    }

    SECTION("setLayout invalidates pending state writes before destroying the previous tree")
    {
      auto stateStore = FakeLayoutComponentStateStore{};
      runtimeState.activePresetId = "classic";
      runtimeState.componentState = LayoutComponentStateDocument{.preset = "classic"};
      runtimeState.componentStateStore = &stateStore;

      auto splitDoc = LayoutDocument{};
      splitDoc.root.id = "main-paned";
      splitDoc.root.type = "split";
      splitDoc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      splitDoc.root.props["initialPositionPercent"] = LayoutValue{0.25};
      splitDoc.root.children.push_back(LayoutNode{.type = "spacer"});
      splitDoc.root.children.push_back(LayoutNode{.type = "spacer"});
      host.setLayout(ctx, splitDoc);

      auto allocationHost = AllocationHost{host};
      allocationHost.allocateChild(1000, 400);
      auto* const paned = ao::gtk::test::findWidget<Gtk::Paned>(host);
      REQUIRE(paned != nullptr);
      paned->set_position(400);

      auto replacement = LayoutDocument{};
      replacement.root.type = "spacer";
      host.setLayout(ctx, replacement);

      CHECK(stateStore.saveCount() == 0);
      CHECK(runtimeState.componentState.components.empty());
    }

    SECTION("setLayout renders registered semantic components")
    {
      auto registry2 = ComponentRegistry{};
      LayoutRuntime::registerStandardComponents(registry2);

      auto window2 = Gtk::Window{};
      auto const tempDir2 = ao::test::TempDir{};
      auto runtime2 = makeRuntime(tempDir2);
      auto const actionRegistry2 = ActionRegistry{};
      auto runtimeState2 = LayoutRuntimeState{};
      auto dependencies2 = GtkUiDependencies{};
      auto ctx2 = LayoutBuildContext{.registry = registry2,
                                     .actionRegistry = actionRegistry2,
                                     .runtime = runtime2,
                                     .parentWindow = window2,
                                     .runtimeState = runtimeState2,
                                     .dependencies = dependencies2};

      auto doc = LayoutDocument{};
      doc.root.type = "status.messageLabel";

      auto host2 = LayoutHost{registry2};
      host2.setLayout(ctx2, doc);

      auto* const label = dynamic_cast<Gtk::Label*>(host2.get_first_child());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Aobus Ready");
    }
  }
} // namespace ao::gtk::layout::test
