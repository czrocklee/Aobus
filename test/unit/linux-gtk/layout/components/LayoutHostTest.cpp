// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/runtime/LayoutHost.h"

#include "app/ShellLayoutCollaborators.h"
#include "app/ThemeCoordinator.h"
#include "app/linux-gtk/layout/component/ComponentRegistrations.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutPresets.h"
#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkLayoutTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include "test/unit/linux-gtk/layout/components/ContainerTestHelpers.h"
#include "test/unit/linux-gtk/layout/state/FakeLayoutComponentStateStore.h"
#include "track/TrackPageHost.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::AllocationHost;
  using ao::gtk::test::makeRuntime;

  namespace
  {
    std::unique_ptr<LayoutComponent> makeFailingComponent(LayoutBuildContext& /*context*/, LayoutNode const& /*node*/)
    {
      throw std::runtime_error{"Test exception"};
    }
  } // namespace

  TEST_CASE("LayoutHost - rebuilds widget trees after layout updates", "[gtk][unit][layout][container]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = ao::test::TempDir{};
    std::unique_ptr<rt::AppRuntime> const runtimePtr = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    registerContainerComponents(registry, ao::test::englishMessageCatalog());
    registry.registerComponent({.id = "test.null", .displayName = "Null"}, makeFailingComponent);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{registry.schema()};
    auto stateStore = FakeLayoutComponentStateStore{};
    auto session = uimodel::LayoutSession{&stateStore};
    auto buildSnapshot = session.buildSnapshot(LayoutComponentStateDocument{.preset = "classic"}, false).value();
    auto ctx = LayoutBuildContext{.registry = registry,
                                  .actionRegistry = actionRegistry,
                                  .parentWindow = window,
                                  .session = session,
                                  .buildSnapshot = std::move(buildSnapshot)};

    auto host = LayoutHost{registry};
    auto install = [&](LayoutDocument const& document)
    {
      auto componentState = session.componentState();
      componentState.preset = "classic";
      ctx.buildSnapshot = session.buildSnapshot(componentState, false).value();
      auto prepared = ao::test::requireValue(prepareLayout(document));
      auto tree = ao::test::requireValue(host.prepare(ctx, prepared));
      session.apply(document, std::move(componentState), tree.generation());
      host.commit(std::move(tree));
    };

    SECTION("Initial layout is empty before commit")
    {
      CHECK(host.get_first_child() == nullptr);
    }

    SECTION("committing a prepared default document populates widget")
    {
      install(makeDefaultLayout());

      auto* const child = host.get_first_child();

      REQUIRE(child != nullptr);
      CHECK(dynamic_cast<Gtk::Widget*>(child) != nullptr);
    }

    SECTION("commit replaces the previous layout")
    {
      install(makeDefaultLayout());

      auto* const first = host.get_first_child();
      CHECK(first != nullptr);

      auto newDoc = LayoutDocument{};
      newDoc.root.type = "spacer";
      install(newDoc);

      auto* const second = host.get_first_child();
      CHECK(second != nullptr);
      CHECK(second != first);
    }

    SECTION("commit invalidates pending state writes before destroying the previous tree")
    {
      auto splitDoc = LayoutDocument{};
      splitDoc.root.id = "main-paned";
      splitDoc.root.type = "split";
      splitDoc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      splitDoc.root.props["initialPositionPercent"] = LayoutValue{0.25};
      splitDoc.root.children.push_back(LayoutNode{.type = "spacer"});
      splitDoc.root.children.push_back(LayoutNode{.type = "spacer"});
      install(splitDoc);

      auto allocationHost = AllocationHost{host};
      allocationHost.allocateChild(1000, 400);
      auto* const paned = ao::gtk::test::findWidget<Gtk::Paned>(host);
      REQUIRE(paned != nullptr);
      paned->set_position(400);

      auto replacement = LayoutDocument{};
      replacement.root.type = "spacer";
      install(replacement);

      CHECK(stateStore.saveCount() == 0);
      CHECK(session.componentState().components.empty());
    }

    SECTION("unexpected component exception preserves the active tree and generation")
    {
      install(makeDefaultLayout());

      auto* const activeChild = host.get_first_child();
      auto const activeGeneration = session.generation();

      auto rejected = LayoutDocument{};
      rejected.root.type = "test.null";
      ctx.buildSnapshot = session.buildSnapshot().value();
      auto prepared = ao::test::requireValue(prepareLayout(rejected));
      CHECK_FALSE(host.prepare(ctx, prepared));
      CHECK(host.get_first_child() == activeChild);
      CHECK(session.generation() == activeGeneration);
    }

    SECTION("discarding a prepared tree preserves the active tree and generation")
    {
      install(makeDefaultLayout());

      auto* const activeChild = host.get_first_child();
      auto const activeGeneration = session.generation();

      auto replacement = LayoutDocument{};
      replacement.root.type = "spacer";
      ctx.buildSnapshot = session.buildSnapshot().value();
      auto prepared = ao::test::requireValue(prepareLayout(replacement));
      auto pending = ao::test::requireValue(host.prepare(ctx, prepared));

      CHECK(pending.generation() == activeGeneration + 1);
      CHECK(host.get_first_child() == activeChild);
      CHECK(session.generation() == activeGeneration);
    }

    SECTION("commit renders registered semantic components")
    {
      auto registry2 = ComponentRegistry{};
      auto window2 = Gtk::Window{};
      auto const tempDir2 = ao::test::TempDir{};
      std::unique_ptr<rt::AppRuntime> runtime2Ptr = makeRuntime(tempDir2);
      LayoutRuntime::registerStandardComponents(
        registry2, *runtime2Ptr, ShellLayoutCollaborators{.textCatalog = ao::test::englishMessageCatalog()});

      auto const actionRegistry2 = ActionRegistry{registry2.schema()};
      auto session2 = uimodel::LayoutSession{};
      auto buildSnapshot2 = session2.buildSnapshot().value();
      auto ctx2 = LayoutBuildContext{.registry = registry2,
                                     .actionRegistry = actionRegistry2,
                                     .parentWindow = window2,
                                     .session = session2,
                                     .buildSnapshot = std::move(buildSnapshot2)};

      auto doc = LayoutDocument{};
      doc.root.type = "status.message";

      auto host2 = LayoutHost{registry2};
      auto prepared = ao::test::requireValue(prepareLayout(doc));
      auto tree = ao::test::requireValue(host2.prepare(ctx2, prepared));
      session2.apply(std::move(doc), {}, tree.generation());
      host2.commit(std::move(tree));

      auto* const label = dynamic_cast<Gtk::Label*>(host2.get_first_child());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Aobus Ready");
    }
  }

  TEST_CASE("LayoutHost - shared track stack handoff is atomic across layout replacement",
            "[gtk][unit][layout][lifetime]")
  {
    [[maybe_unused]] auto const appPtr = Gtk::Application::create("io.github.aobus.layout_handoff_test");
    auto runtimeFixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = runtimeFixture.runtime();
    auto window = Gtk::Window{};
    auto stack = Gtk::Stack{};
    auto themeCoordinator = ThemeCoordinator{};
    auto tagEditController = TagEditController{
      window, runtime, ao::test::englishMessageCatalog(), TagEditController::Callbacks{}, themeCoordinator};
    auto listNavigation = ListNavigationController{
      window, runtime, ao::test::englishMessageCatalog(), ListNavigationController::Callbacks{}, themeCoordinator};
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto trackPageHost = TrackPageHost{stack,
                                       runtime,
                                       tagEditController,
                                       listNavigation,
                                       columnLayouts,
                                       ao::test::englishMessageCatalog(),
                                       runtime.resourceBytes()};

    auto registry = ComponentRegistry{};
    registerContainerComponents(registry, ao::test::englishMessageCatalog());
    registerTracksTableComponent(registry, &trackPageHost);
    registerWorkspaceWithDetailPaneComponent(registry, &trackPageHost, ao::test::englishMessageCatalog());
    registry.registerComponent({.id = "test.fail", .displayName = "Fail"}, makeFailingComponent);

    auto actions = ActionRegistry{registry.schema()};
    auto session = uimodel::LayoutSession{};
    auto buildSnapshot = session.buildSnapshot().value();
    auto context = LayoutBuildContext{.registry = registry,
                                      .actionRegistry = actions,
                                      .parentWindow = window,
                                      .session = session,
                                      .buildSnapshot = std::move(buildSnapshot)};
    auto host = LayoutHost{registry};
    auto install = [&](LayoutDocument const& document)
    {
      context.buildSnapshot = session.buildSnapshot().value();
      auto prepared = ao::test::requireValue(prepareLayout(document));
      auto tree = ao::test::requireValue(host.prepare(context, prepared));
      session.apply(document, {}, tree.generation());
      host.commit(std::move(tree));
    };

    auto tableDocument = LayoutDocument{};
    tableDocument.root.type = "track.table";
    install(tableDocument);
    auto* const tableParent = stack.get_parent();
    REQUIRE(tableParent != nullptr);
    CHECK(tableParent->get_parent() == &host);

    auto workspaceDocument = LayoutDocument{};
    workspaceDocument.root.type = "workspace.withDetailPane";
    install(workspaceDocument);
    auto* const workspaceParent = stack.get_parent();
    REQUIRE(workspaceParent != nullptr);
    CHECK(workspaceParent != tableParent);
    CHECK(workspaceParent->get_parent() == &host);

    install(tableDocument);
    auto* const restoredTableParent = stack.get_parent();
    REQUIRE(restoredTableParent != nullptr);
    CHECK(restoredTableParent != workspaceParent);
    CHECK(restoredTableParent->get_parent() == &host);

    auto rejectedDocument = LayoutDocument{};
    rejectedDocument.root.type = "box";
    rejectedDocument.root.children = {
      LayoutNode{.type = "workspace.withDetailPane"},
      LayoutNode{.type = "test.fail"},
    };
    context.buildSnapshot = session.buildSnapshot().value();
    auto const generationBeforeFailure = session.generation();
    auto* const activeRootBeforeFailure = host.get_first_child();
    auto rejectedPrepared = ao::test::requireValue(prepareLayout(rejectedDocument));

    CHECK_FALSE(host.prepare(context, rejectedPrepared));
    CHECK(session.generation() == generationBeforeFailure);
    CHECK(host.get_first_child() == activeRootBeforeFailure);
    CHECK(stack.get_parent() == restoredTableParent);
    CHECK(restoredTableParent->get_parent() == &host);

    install(workspaceDocument);
    REQUIRE(stack.get_parent() != nullptr);
    CHECK(stack.get_parent()->get_parent() == &host);
  }
} // namespace ao::gtk::layout::test
