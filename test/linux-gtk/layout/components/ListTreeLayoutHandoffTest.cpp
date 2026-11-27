// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/ThemeCoordinator.h"
#include "layout/component/ComponentRegistrations.h"
#include "layout/document/LayoutDialect.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutHost.h"
#include "list/ListNavigationController.h"
#include "list/ListTreeItem.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkLayoutTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/document/LayoutValidation.h>
#include <ao/uimodel/layout/shell/LayoutSession.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <gdkmm/frameclock.h>
#include <glib-object.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/centerbox.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listview.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistrow.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace ao::gtk::layout::test
{
  namespace
  {
    void requireAfterPaint(Gtk::Window& window)
    {
      auto const clockPtr = window.get_frame_clock();
      REQUIRE(clockPtr);
      bool reachedAfterPaint = false;
      auto const handler = ::g_signal_connect_data(
        clockPtr->gobj(),
        "after-paint",
        G_CALLBACK(+[](::GdkFrameClock*, ::gpointer data) { *static_cast<bool*>(data) = true; }),
        &reachedAfterPaint,
        nullptr,
        G_CONNECT_AFTER);
      REQUIRE(handler != 0);
      auto const disconnect =
        utility::ScopedRegistration{[&] { ::g_signal_handler_disconnect(clockPtr->gobj(), handler); }};
      window.queue_draw();
      clockPtr->request_phase(Gdk::FrameClock::Phase::PAINT | Gdk::FrameClock::Phase::AFTER_PAINT);
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return reachedAfterPaint; }));
    }
  } // namespace

  TEST_CASE("ListTree - layout handoff preserves the live navigation model and selection")
  {
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto runtimeFixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = runtimeFixture.runtime();
    auto const listId = ao::test::requireValue(ao::gtk::test::runGtkTask(
      runtime, runtimeFixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Selected List"})));
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
    auto windowFixture = ao::gtk::test::GtkWindowFixture{};
    auto& window = windowFixture.window();
    auto themeCoordinator = ThemeCoordinator{};
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    bool selectionRequested = false;
    auto navigation = ListNavigationController{window,
                                               runtime,
                                               runtimeFixture.editing(),
                                               ao::test::englishMessageCatalog(),
                                               {.onListSelected =
                                                  [&](ListId)
                                                {
                                                  selectionRequested = true;
                                                  return true;
                                                }},
                                               themeCoordinator};
    navigation.rebuildTree(rowCache);
    auto& panel = dynamic_cast<Gtk::ScrolledWindow&>(navigation.widget());
    auto const panelVexpand = panel.get_vexpand();
    auto const panelVexpandSet = panel.get_vexpand_set();
    auto const panelHexpand = panel.get_hexpand();
    auto const panelHexpandSet = panel.get_hexpand_set();

    auto registry = ComponentRegistry{};
    registerContainerComponents(registry, ao::test::englishMessageCatalog());
    registerLabelComponent(registry);
    registerListTreeComponent(registry, &navigation);

    auto const actions = ActionRegistry{registry.schema()};
    auto session = uimodel::LayoutSession{};
    auto context = LayoutBuildContext{.registry = registry,
                                      .actionRegistry = actions,
                                      .parentWindow = window,
                                      .session = session,
                                      .buildSnapshot = session.buildSnapshot().value()};
    auto host = LayoutHost{registry};
    auto document = uimodel::LayoutDocument{};
    document.root.type = "library.listTree";
    auto const prepared = ao::test::requireValue(uimodel::prepareLayout(document));
    auto initialTree = ao::test::requireValue(host.prepare(context, prepared));
    session.apply(document, {}, initialTree.generation());
    host.commit(std::move(initialTree));

    auto* const listView = dynamic_cast<Gtk::ListView*>(panel.get_child());
    REQUIRE(listView != nullptr);
    auto const selectionPtr = std::dynamic_pointer_cast<Gtk::SingleSelection>(listView->get_model());
    REQUIRE(selectionPtr != nullptr);
    auto* const originalRoot = host.get_first_child();
    REQUIRE(originalRoot != nullptr);
    auto const originalGeneration = session.generation();
    auto const selectedList = [&]
    {
      auto const rowPtr = std::dynamic_pointer_cast<Gtk::TreeListRow>(selectionPtr->get_selected_item());
      REQUIRE(rowPtr != nullptr);
      auto const itemPtr = std::dynamic_pointer_cast<ListTreeItem>(rowPtr->get_item());
      REQUIRE(itemPtr != nullptr);
      return itemPtr->listId();
    };
    REQUIRE(selectedList() == listId);
    CHECK_FALSE(selectionRequested);

    auto candidate = document;
    candidate.root.layout["visible"] = uimodel::LayoutValue{false};
    context.buildSnapshot = session.buildSnapshot().value();

    auto focusSibling = Gtk::Button{"Other focus target"};
    auto focusBox = Gtk::Box{Gtk::Orientation::VERTICAL};
    auto const focusNavigation = [&]
    {
      focusBox.append(host);
      focusBox.append(focusSibling);
      windowFixture.mount(focusBox);
      windowFixture.present();
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return listView->get_mapped(); }));
      REQUIRE(focusSibling.grab_focus());
      REQUIRE(window.get_focus() == &focusSibling);
      REQUIRE(listView->grab_focus());
      auto* const focus = window.get_focus();
      REQUIRE(focus != nullptr);
      REQUIRE(focus->is_ancestor(panel));
      return focus;
    };
    auto const checkLiveNavigation = [&](Gtk::Widget const* focus)
    {
      CHECK(host.get_first_child() == originalRoot);
      CHECK(session.generation() == originalGeneration);
      CHECK(panel.get_parent() == originalRoot);
      CHECK(panel.get_mapped());
      CHECK(listView->get_model() == selectionPtr);
      CHECK(selectedList() == listId);
      CHECK(window.get_focus() == focus);
    };
    auto const detachHost = utility::ScopedRegistration{[&] { windowFixture.unmount(); }};

    SECTION("a failure after preparing a list tree never disturbs the focused control")
    {
      auto const* const focus = focusNavigation();
      bool reachedFailure = false;
      registry.registerComponent(
        {.id = "test.fail", .displayName = "Fail"},
        [&](LayoutBuildContext&, uimodel::LayoutNode const&) -> std::unique_ptr<LayoutComponent>
        {
          reachedFailure = true;
          checkLiveNavigation(focus);
          throw std::runtime_error{"Rejected list-tree candidate"};
        });
      candidate.root.optTooltip = uimodel::BoxedLayoutNode{uimodel::LayoutNode{.type = "test.fail"}};
      auto const rejected = ao::test::requireValue(uimodel::prepareLayout(candidate));
      auto failedRes = host.prepare(context, rejected);
      REQUIRE_FALSE(failedRes);
      CHECK(failedRes.error().code == Error::Code::InitFailed);
      REQUIRE(reachedFailure);
      requireAfterPaint(window);
      checkLiveNavigation(focus);
    }

    SECTION("preparing and discarding a hidden candidate leaves the live control focused and mapped")
    {
      auto const* const focus = focusNavigation();
      auto const hidden = ao::test::requireValue(uimodel::prepareLayout(candidate));
      {
        auto pending = ao::test::requireValue(host.prepare(context, hidden));
        CHECK(pending.generation() == originalGeneration + 1);
        checkLiveNavigation(focus);
        requireAfterPaint(window);
        checkLiveNavigation(focus);
      }
      requireAfterPaint(window);
      checkLiveNavigation(focus);
    }

    SECTION("commit hides only the replacement wrapper and not the controller's own widget")
    {
      auto const hidden = ao::test::requireValue(uimodel::prepareLayout(candidate));
      auto pending = ao::test::requireValue(host.prepare(context, hidden));
      session.apply(candidate, {}, pending.generation());
      host.commit(std::move(pending));

      auto* const root = host.get_first_child();
      REQUIRE(root != nullptr);
      CHECK(root != originalRoot);
      CHECK(panel.get_parent() == root);
      CHECK_FALSE(root->get_visible());
    }

    SECTION("the authored expanding child takes the remaining height without changing the scroller flags")
    {
      auto const listExpands = GENERATE(true, false);
      CAPTURE(listExpands);

      auto nested = uimodel::LayoutDocument{};
      nested.root.type = "box";
      nested.root.children = {
        uimodel::LayoutNode{.type = "library.listTree",
                            .layout = {{"heightRequest", uimodel::LayoutValue{std::int64_t{70}}},
                                       {"vexpand", uimodel::LayoutValue{listExpands}}}},
        uimodel::LayoutNode{.type = "spacer",
                            .layout = {{"heightRequest", uimodel::LayoutValue{std::int64_t{30}}},
                                       {"vexpand", uimodel::LayoutValue{!listExpands}}}},
      };
      auto const replacement = ao::test::requireValue(uimodel::prepareLayout(nested));
      auto pending = ao::test::requireValue(host.prepare(context, replacement));
      session.apply(nested, {}, pending.generation());
      host.commit(std::move(pending));

      auto* const root = host.get_first_child();
      REQUIRE(root != nullptr);
      auto* const wrapper = root->get_first_child();
      auto* const sibling = root->get_last_child();
      REQUIRE(wrapper != nullptr);
      REQUIRE(sibling != nullptr);
      REQUIRE(wrapper != sibling);
      CHECK(panel.get_parent() == wrapper);
      CHECK(wrapper->get_vexpand() == listExpands);
      CHECK(wrapper->get_vexpand_set());
      auto allocationHost = ao::gtk::test::AllocationHost{host};
      allocationHost.allocateChild(320, 480);
      CHECK(panel.get_width() == wrapper->get_width());
      CHECK(panel.get_height() == wrapper->get_height());
      CHECK(panel.get_height() > 0);
      CHECK(sibling->get_height() >= 30);
      CHECK(wrapper->get_height() + sibling->get_height() == root->get_height());
      CHECK((wrapper->get_height() > sibling->get_height()) == listExpands);
    }

    SECTION("a candidate with two list trees is rejected and the active widget stays attached")
    {
      auto const useCenterBox = GENERATE(false, true);
      CAPTURE(useCenterBox);
      auto duplicate = uimodel::LayoutDocument{};
      duplicate.root.type = useCenterBox ? "centerBox" : "box";
      duplicate.root.children = {document.root, document.root};

      if (useCenterBox)
      {
        duplicate.root.children.front().layout["slot"] = uimodel::LayoutValue{"start"};
        duplicate.root.children.back().layout["slot"] = uimodel::LayoutValue{"center"};
      }

      auto const rejected = ao::test::requireValue(uimodel::prepareLayout(duplicate));
      REQUIRE(uimodel::requireValidLayout(rejected, registry.schema(), layoutDialect()));
      auto duplicateRes = host.prepare(context, rejected);
      REQUIRE_FALSE(duplicateRes);
      CHECK(duplicateRes.error().code == Error::Code::InitFailed);
      CHECK(duplicateRes.error().message.contains("may appear only once"));
      CHECK(host.get_first_child() == originalRoot);
      CHECK(session.generation() == originalGeneration);
      CHECK(panel.get_parent() == originalRoot);
      CHECK(originalRoot->get_visible());
    }

    SECTION("a displaced centerBox subtree does not keep a list handoff")
    {
      auto const retainList = GENERATE(false, true);
      auto replacement = uimodel::LayoutDocument{};
      replacement.root.type = "centerBox";
      replacement.root.children = {
        uimodel::LayoutNode{
          .type = "box", .layout = {{"slot", uimodel::LayoutValue{"start"}}}, .children = {document.root}},
        uimodel::LayoutNode{.type = "label",
                            .props = {{"text", uimodel::LayoutValue{"Replacement"}}},
                            .layout = {{"slot", uimodel::LayoutValue{"start"}}}},
      };

      if (retainList)
      {
        auto retained = document.root;
        retained.layout["slot"] = uimodel::LayoutValue{"end"};
        replacement.root.children.push_back(std::move(retained));
      }

      auto const preparedReplacement = ao::test::requireValue(uimodel::prepareLayout(replacement));
      auto pending = ao::test::requireValue(host.prepare(context, preparedReplacement));
      CHECK(panel.get_parent() == originalRoot);
      session.apply(replacement, {}, pending.generation());
      host.commit(std::move(pending));

      auto* const centerBox = dynamic_cast<Gtk::CenterBox*>(host.get_first_child());
      REQUIRE(centerBox != nullptr);
      auto const* const start = dynamic_cast<Gtk::Label*>(centerBox->get_start_widget());
      REQUIRE(start != nullptr);
      CHECK(start->get_text() == "Replacement");
      auto* const end = centerBox->get_end_widget();
      CHECK((end != nullptr) == retainList);
      CHECK(panel.get_parent() == end);
    }

    SECTION("only lists with a centerBox slot claim the shared panel")
    {
      auto slotted = document.root;
      slotted.layout["slot"] = uimodel::LayoutValue{"start"};
      auto centerLayout = uimodel::LayoutDocument{};
      centerLayout.root.type = "centerBox";
      centerLayout.root.children = {slotted, document.root};
      auto const useExplicitEmptySlot = GENERATE(false, true);
      CAPTURE(useExplicitEmptySlot);

      if (useExplicitEmptySlot)
      {
        centerLayout.root.children.back().layout["slot"] = uimodel::LayoutValue{""};
      }

      auto const replacement = ao::test::requireValue(uimodel::prepareLayout(centerLayout));
      REQUIRE(uimodel::requireValidLayout(replacement, registry.schema(), layoutDialect()));
      auto pending = ao::test::requireValue(host.prepare(context, replacement));
      CHECK(panel.get_parent() == originalRoot);
      session.apply(centerLayout, {}, pending.generation());
      host.commit(std::move(pending));

      auto* const centerBox = dynamic_cast<Gtk::CenterBox*>(host.get_first_child());
      REQUIRE(centerBox != nullptr);
      auto* const start = centerBox->get_start_widget();
      REQUIRE(start != nullptr);
      CHECK(panel.get_parent() == start);
      CHECK(centerBox->get_center_widget() == nullptr);
      CHECK(centerBox->get_end_widget() == nullptr);
    }

    SECTION("committing a layout without a list tree detaches the unused panel")
    {
      auto withoutList = uimodel::LayoutDocument{};
      withoutList.root.type = "spacer";
      auto const replacement = ao::test::requireValue(uimodel::prepareLayout(withoutList));
      auto pending = ao::test::requireValue(host.prepare(context, replacement));
      CHECK(panel.get_parent() == originalRoot);
      session.apply(withoutList, {}, pending.generation());
      host.commit(std::move(pending));

      REQUIRE(host.get_first_child() != nullptr);
      CHECK(host.get_first_child() != originalRoot);
      CHECK(panel.get_parent() == nullptr);
    }

    CHECK(panel.get_visible());
    CHECK(panel.get_vexpand() == panelVexpand);
    CHECK(panel.get_vexpand_set() == panelVexpandSet);
    CHECK(panel.get_hexpand() == panelHexpand);
    CHECK(panel.get_hexpand_set() == panelHexpandSet);
    CHECK(listView->get_model() == selectionPtr);
    CHECK(selectedList() == listId);
    CHECK(runtime.workspace().snapshot().activeViewId == viewId);
    CHECK_FALSE(selectionRequested);
    host.clearLayout();
    CHECK(panel.get_parent() == nullptr);
  }
} // namespace ao::gtk::layout::test
