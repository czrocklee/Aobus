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
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkLayoutTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/linux-gtk/layout/LayoutTestSupport.h"
#include "test/linux-gtk/layout/components/ContainerTestHelpers.h"
#include "test/linux-gtk/layout/state/FakeLayoutComponentStateStore.h"
#include "track/TrackPageHost.h"
#include "track/TrackRowCache.h"
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <gtkmm/application.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/revealer.h>
#include <gtkmm/stack.h>
#include <gtkmm/togglebutton.h>
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

  TEST_CASE("LayoutHost - commit replaces widget trees and fences stale state writes")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

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
    // LayoutHost does not own LayoutSession advancement. This lower-level
    // fixture deliberately models the caller-owned apply-before-commit order.
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
      auto ownerPtr = makeRuntime(tempDir2);
      LayoutRuntime::registerStandardComponents(
        registry2,
        ownerPtr->runtime(),
        uimodel::LibraryEditing{ownerPtr->libraryCommands()},
        ShellLayoutCollaborators{.textCatalog = ao::test::englishMessageCatalog(),
                                 .outputDeviceIntent = uimodel::OutputDeviceIntent::discarded()});

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
      CHECK(label->get_text().raw() ==
            i18n::requiredText(ao::test::englishMessageCatalog(), i18n::MessageId::CommonReady));
    }
  }

  TEST_CASE("LayoutHost - shared track stack handoff is atomic across layout replacement")
  {
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto runtimeFixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = runtimeFixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto window = Gtk::Window{};
    auto stack = Gtk::Stack{};
    auto themeCoordinator = ThemeCoordinator{};
    auto tagEditController = TagEditController{window,
                                               runtime.async(),
                                               runtimeFixture.editing(),
                                               runtime.completion(),
                                               runtime.notifications(),
                                               runtime.textOrderingPolicy(),
                                               ao::test::englishMessageCatalog(),
                                               TagEditController::Callbacks{},
                                               themeCoordinator};
    auto listNavigation = ListNavigationController{window,
                                                   runtime,
                                                   runtimeFixture.editing(),
                                                   ao::test::englishMessageCatalog(),
                                                   ListNavigationController::Callbacks{},
                                                   themeCoordinator};
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto trackPageHost = TrackPageHost{stack,
                                       runtime.async(),
                                       runtimeFixture.editing(),
                                       runtime.playback(),
                                       runtime.views(),
                                       runtime.workspace(),
                                       tagEditController,
                                       listNavigation,
                                       columnLayouts,
                                       ao::test::englishMessageCatalog(),
                                       runtime.resourceBytes()};

    auto registry = ComponentRegistry{};
    registerContainerComponents(registry, ao::test::englishMessageCatalog());
    registerTracksTableComponent(registry, &trackPageHost);
    registerWorkspaceWithDetailPaneComponent(registry, &trackPageHost, ao::test::englishMessageCatalog());
    registerLabelComponent(registry);
    bool reachedFailure = false;
    registry.registerComponent({.id = "test.fail", .displayName = "Fail"},
                               [&](LayoutBuildContext& context, LayoutNode const& node)
                               {
                                 reachedFailure = true;
                                 return makeFailingComponent(context, node);
                               });

    auto actions = ActionRegistry{registry.schema()};
    auto session = uimodel::LayoutSession{};
    auto buildSnapshot = session.buildSnapshot().value();
    auto context = LayoutBuildContext{.registry = registry,
                                      .actionRegistry = actions,
                                      .parentWindow = window,
                                      .session = session,
                                      .buildSnapshot = std::move(buildSnapshot)};
    auto host = LayoutHost{registry};
    auto prepare = [&](LayoutDocument const& document)
    {
      auto optSnapshot = session.buildSnapshot();
      REQUIRE(optSnapshot);
      context.buildSnapshot = std::move(*optSnapshot);
      auto prepared = ao::test::requireValue(prepareLayout(document));
      return host.prepare(context, prepared);
    };
    auto commit = [&](LayoutDocument const& document, LayoutHost::PreparedTree tree)
    {
      session.apply(document, {}, tree.generation());
      host.commit(std::move(tree));
    };
    auto checkPreparationFailure = [&](LayoutNode const& candidate)
    {
      auto rejected = LayoutDocument{};
      rejected.root.type = "box";
      rejected.root.children = {candidate, LayoutNode{.type = "test.fail"}};
      auto res = prepare(rejected);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InitFailed);
      CHECK(reachedFailure);
    };

    using Style = CoverArtPlaceholderStyle;
    auto tableDocument = LayoutDocument{};
    tableDocument.root.type = "track.table";
    tableDocument.root.props["groupCoverPlaceholderStyle"] = LayoutValue{"vinyl"};
    commit(tableDocument, ao::test::requireValue(prepare(tableDocument)));
    auto* const tableParent = stack.get_parent();
    REQUIRE(tableParent != nullptr);
    REQUIRE(host.get_first_child() == tableParent);
    auto const activeGeneration = session.generation();

    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    trackPageHost.rebuild(cache);
    ao::gtk::test::drainGtkEvents();
    auto const* const activePage = trackPageHost.find(viewId);
    REQUIRE(activePage != nullptr);
    auto const* const initialPage = activePage->pagePtr.get();
    REQUIRE(initialPage != nullptr);
    auto checkStyle = [&](Style style)
    {
      CHECK(trackPageHost.groupCoverPlaceholderStyle() == style);
      auto const* const page = trackPageHost.find(viewId);
      REQUIRE(page != nullptr);
      REQUIRE(page->pagePtr.get() == initialPage);
      CHECK(page->pagePtr->groupCoverPlaceholderStyle() == style);
    };
    checkStyle(Style::Vinyl);

    bool parentChanged = false;
    auto const parentConnection = sigc::scoped_connection{
      stack.property_parent().signal_changed().connect([&parentChanged] { parentChanged = true; })};
    auto checkActiveTree = [&]
    {
      CHECK(session.generation() == activeGeneration);
      CHECK(host.get_first_child() == tableParent);
      CHECK(stack.get_parent() == tableParent);
      CHECK_FALSE(parentChanged);
    };

    auto styledTable = tableDocument;
    styledTable.root.props["groupCoverPlaceholderStyle"] = LayoutValue{"soul"};
    auto workspaceDocument = LayoutDocument{};
    workspaceDocument.root.type = "workspace.withDetailPane";
    auto detail = LayoutNode{.type = "label"};
    detail.props["text"] = LayoutValue{"Track details"};
    workspaceDocument.root.children.push_back(std::move(detail));

    SECTION("a later component failure does not change the live table style")
    {
      checkPreparationFailure(styledTable.root);
      checkActiveTree();
      checkStyle(Style::Vinyl);
    }

    SECTION("a pending table keeps the live style until committed")
    {
      auto candidate = ao::test::requireValue(prepare(styledTable));
      checkActiveTree();
      checkStyle(Style::Vinyl);
      commit(styledTable, std::move(candidate));
      CHECK(host.get_first_child() != tableParent);
      CHECK(stack.get_parent() == host.get_first_child());
      CHECK(session.generation() == activeGeneration + 1);
      checkStyle(Style::Soul);
    }

    SECTION("discarding a table candidate preserves the live style and parent")
    {
      {
        auto const candidate = ao::test::requireValue(prepare(styledTable));
        CHECK(candidate.generation() == activeGeneration + 1);
        checkActiveTree();
        checkStyle(Style::Vinyl);
      }
      checkActiveTree();
      checkStyle(Style::Vinyl);
    }

    SECTION("an abandoned workspace preserves stack expansion and explicit-set flags")
    {
      // Exercise each axis with both a changed value and a changed explicit-set bit.
      auto const expandHorizontally = GENERATE(false, true);
      stack.set_hexpand(expandHorizontally);
      stack.set_vexpand(!expandHorizontally);
      stack.set_hexpand_set(!expandHorizontally);
      stack.set_vexpand_set(expandHorizontally);

      SECTION("a later component throws")
      {
        checkPreparationFailure(workspaceDocument.root);
      }

      SECTION("a prepared tree is discarded")
      {
        auto const candidate = ao::test::requireValue(prepare(workspaceDocument));
        CHECK(candidate.generation() == activeGeneration + 1);
        checkActiveTree();
      }

      checkActiveTree();
      checkStyle(Style::Vinyl);
      CHECK(stack.get_hexpand() == expandHorizontally);
      CHECK(stack.get_hexpand_set() == !expandHorizontally);
      CHECK(stack.get_vexpand() == !expandHorizontally);
      CHECK(stack.get_vexpand_set() == expandHorizontally);
    }

    SECTION("a table replacement preserves expansion from an accepted workspace")
    {
      stack.set_hexpand(false);
      stack.set_vexpand(false);
      auto candidate = ao::test::requireValue(prepare(workspaceDocument));
      checkActiveTree();
      commit(workspaceDocument, std::move(candidate));
      auto* const workspaceParent = stack.get_parent();
      REQUIRE(workspaceParent != nullptr);
      CHECK(workspaceParent != tableParent);
      CHECK(workspaceParent == host.get_first_child());

      // The track stack precedes the functional detail controls in the horizontal workspace.
      Gtk::ToggleButton* handle = nullptr;
      Gtk::Revealer* revealer = nullptr;

      for (auto* sibling = stack.get_next_sibling(); sibling != nullptr; sibling = sibling->get_next_sibling())
      {
        if (auto* const toggle = dynamic_cast<Gtk::ToggleButton*>(sibling); toggle != nullptr)
        {
          handle = toggle;
        }

        if (auto* const candidateRevealer = dynamic_cast<Gtk::Revealer*>(sibling);
            candidateRevealer != nullptr &&
            ao::gtk::test::findLabelByText(*candidateRevealer, "Track details") != nullptr)
        {
          revealer = candidateRevealer;
        }
      }

      REQUIRE(handle != nullptr);
      REQUIRE(revealer != nullptr);
      CHECK_FALSE(revealer->get_reveal_child());
      handle->set_active(true);
      CHECK(revealer->get_reveal_child());

      CHECK(stack.get_hexpand());
      CHECK(stack.get_hexpand_set());
      CHECK(stack.get_vexpand());
      CHECK(stack.get_vexpand_set());
      checkStyle(Style::Vinyl);

      // track.table changes the placeholder style, not the shared stack's expansion.
      commit(styledTable, ao::test::requireValue(prepare(styledTable)));
      CHECK(stack.get_parent() != workspaceParent);
      CHECK(stack.get_parent() == host.get_first_child());
      CHECK(stack.get_hexpand());
      CHECK(stack.get_hexpand_set());
      CHECK(stack.get_vexpand());
      CHECK(stack.get_vexpand_set());
      checkStyle(Style::Soul);
    }
  }
} // namespace ao::gtk::layout::test
