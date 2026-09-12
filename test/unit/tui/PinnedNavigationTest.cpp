// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/tui/EventControllerTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/EventController.h"
#include "tui/HitRegions.h"
#include "tui/Keymap.h"
#include "tui/LibraryChooser.h"
#include "tui/LibraryController.h"
#include "tui/ListNavigationModel.h"
#include "tui/ListSearch.h"
#include "tui/MouseBindings.h"
#include "tui/NavigationPanel.h"
#include "tui/PanelWidths.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include "tui/Style.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <cstdint>
#include <list>
#include <string>
#include <utility>

namespace ao::tui::test
{
  namespace
  {
    void updateGeometry(EventControllerFixture& fixture)
    {
      fixture.hitRegions.navigationLayout = navigationGeometry(140, 0, fixture.shell.isNavigationPinned());
    }
  } // namespace

  TEST_CASE("PinnedNavigation - access never changes pin preference and pin promotes the popup",
            "[tui][unit][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    updateGeometry(fixture);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('l')));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('l')));
    CHECK(fixture.shell.isNavigationFocused());
    CHECK(fixture.shell.isNavigationPinned());
    CHECK(fixture.layoutCheckpointCount == 0);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('L')));
    CHECK_FALSE(fixture.shell.isNavigationPinned());
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(fixture.layoutCheckpointCount == 1);
    updateGeometry(fixture);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('l')));
    updateGeometry(fixture);
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK_FALSE(fixture.shell.isNavigationPinned());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('L')));
    updateGeometry(fixture);
    CHECK(fixture.hitRegions.navigationLayout.docked);
    CHECK(fixture.shell.isNavigationFocused());
    CHECK(fixture.layoutCheckpointCount == 2);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(fixture.shell.isNavigationPinned());
  }

  TEST_CASE("PinnedNavigation - divider controls collapse and expand without navigating",
            "[tui][unit][navigation][mouse]")
  {
    using namespace ftxui;
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    auto const previous = library.activeViewId();
    updateGeometry(fixture);
    auto navigationPtr = navigationPanel(ao::test::englishMessageCatalog(),
                                         library.navigation(),
                                         library.currentListId(),
                                         {.regions = &fixture.hitRegions.navigation});
    auto const docked = renderElement(
      dockNavigationPanel(
        std::move(navigationPtr), style::titledPanel("", text("Tracks")), 26, &fixture.hitRegions.navigationPinBox),
      140,
      12);
    INFO(docked.text);
    CHECK(docked.text.contains("‹"));
    CHECK(fixture.hitRegions.navigationPinBox.x_min == 25);
    CHECK(fixture.hitRegions.navigationPinBox.y_min >= 5);
    CHECK(fixture.hitRegions.navigationPinBox.y_max <= 6);
    REQUIRE(events.tryHandleEvent(clickBox(fixture.hitRegions.navigationPinBox)));
    CHECK_FALSE(fixture.shell.isNavigationPinned());
    updateGeometry(fixture);
    auto const collapsed = renderElement(
      collapsedNavigationPanel(style::titledPanel("", text("Tracks")), fixture.hitRegions.navigationPinBox), 140, 12);
    CHECK(collapsed.text.contains("›"));
    CHECK(fixture.hitRegions.navigationPinBox.x_min == 0);
    REQUIRE(events.tryHandleEvent(clickBox(fixture.hitRegions.navigationPinBox)));
    CHECK(fixture.shell.isNavigationPinned());
    CHECK(library.activeViewId() == previous);
    CHECK(fixture.layoutCheckpointCount == 2);
  }

  TEST_CASE("PinnedNavigation - popup dismissal and mouse activation preserve an unpinned layout",
            "[tui][unit][navigation][mouse]")
  {
    auto fixture = EventControllerFixture{};
    auto const target = fixture.addList("Popup target");
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.setNavigationPinned(false);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('l')));
    updateGeometry(fixture);
    auto const popup = renderElement(mousePanel(libraryChooserPane(ao::test::englishMessageCatalog(),
                                                                   library.libraryLabels(),
                                                                   library.libraryEntries(),
                                                                   library.selectedList(),
                                                                   defaultKeymapPlan(),
                                                                   48,
                                                                   fixture.shell.listSearch(),
                                                                   fixture.hitRegions.libraryRows,
                                                                   fixture.hitRegions.overlayPanel.navigationBox),
                                                fixture.hitRegions.overlayPanel),
                                     48,
                                     10);
    INFO(popup.text);
    REQUIRE(fixture.shell.overlay() == Overlay::ListChooser);
    REQUIRE(fixture.hitRegions.navigationLayout.canDock);

    SECTION("An outside click only dismisses the popup")
    {
      auto const box = fixture.hitRegions.overlayPanel.box;
      REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse(
        "", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = box.x_max + 1, .y = box.y_min})));
    }

    SECTION("A popup row closes the popup even in a wide terminal")
    {
      auto const& rows = fixture.hitRegions.libraryRows;
      auto const hit = std::ranges::find(rows, target, &LibraryRowHitRegion::id);
      REQUIRE(hit != rows.end());
      REQUIRE(events.tryHandleEvent(clickBox(hit->box)));
      CHECK(library.currentListId() == target);
    }

    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK_FALSE(fixture.shell.isNavigationPinned());
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(fixture.layoutCheckpointCount == 0);
  }

  TEST_CASE("PinnedNavigation - chooser search with no matches clears targets and cannot activate",
            "[tui][regression][navigation][search]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.setNavigationPinned(false);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('l')));
    auto const activeView = library.activeViewId();
    auto render = [&](i18n::MessageCatalog const& catalog, std::int32_t const terminalColumns)
    {
      auto const columns =
        libraryChooserPaneColumns(catalog, library.libraryLabels(), defaultKeymapPlan(), terminalColumns);
      REQUIRE(columns > 0);
      REQUIRE(columns <= terminalColumns);
      return renderElement(mousePanel(libraryChooserPane(catalog,
                                                         library.libraryLabels(),
                                                         library.libraryEntries(),
                                                         library.selectedList(),
                                                         defaultKeymapPlan(),
                                                         columns,
                                                         fixture.shell.listSearch(),
                                                         fixture.hitRegions.libraryRows,
                                                         fixture.hitRegions.overlayPanel.navigationBox),
                                      fixture.hitRegions.overlayPanel),
                           columns,
                           10);
    };
    render(ao::test::englishMessageCatalog(), 80);
    REQUIRE_FALSE(fixture.hitRegions.libraryRows.empty());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('/')));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character("unmatched-list-query")));

    for (auto const* locale : {"en", "zh-Hant", "de"})
    {
      auto const catalog = ao::test::messageCatalog(locale);

      for (auto const columns : {24, 80})
      {
        auto const rendered = render(catalog, columns);
        CAPTURE(locale, columns, rendered.text);
        CHECK(rendered.text.contains(i18n::requiredText(catalog, i18n::MessageId::TuiListSearchEmpty)));
        CHECK(fixture.hitRegions.libraryRows.empty());
        CHECK_FALSE(fixture.hitRegions.overlayPanel.navigationBox.IsEmpty());
        CHECK(fixture.hitRegions.overlayPanel.navigationBox.x_min >= 0);
        CHECK(fixture.hitRegions.overlayPanel.navigationBox.x_max < columns);
      }
    }

    REQUIRE(events.tryHandleEvent(ftxui::Event::Return));
    CHECK(library.activeViewId() == activeView);
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(fixture.shell.listSearch().isActive());
  }

  TEST_CASE("PinnedNavigation - status advertises popup only when the pinned pane is absent", "[tui][unit][navigation]")
  {
    for (bool const docked : {false, true})
    {
      auto actions = std::list<StatusActionHitRegion>{};
      auto const rendered =
        renderElement(statusBar(ao::test::englishMessageCatalog(),
                                {.terminalColumns = 140, .navigationDocked = docked, .actionHitRegions = &actions},
                                defaultKeymapPlan()),
                      140,
                      1);
      INFO(rendered.text);
      CHECK(std::ranges::any_of(actions, [](auto const& hit) { return hit.action == KeyAction::ToggleLists; }) ==
            !docked);
    }
  }

  TEST_CASE("PinnedNavigation - pin binding can be remapped independently from popup access", "[tui][unit][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({{"tui.workspace.togglePinnedLists", {"F4"}}});
    auto const plan = KeymapPlan{model};
    auto events = fixture.makeEvents(library, plan);
    updateGeometry(fixture);
    REQUIRE(events.tryHandleEvent(ftxui::Event::F4));
    CHECK_FALSE(fixture.shell.isNavigationPinned());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('l')));
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK_FALSE(fixture.shell.isNavigationPinned());
    CHECK(plan.shortcutFor(KeyAction::TogglePinnedLists) == "F4");
  }

  TEST_CASE("PinnedNavigation - restored frame button opens the chooser and hidden targets are cleared",
            "[tui][regression][navigation][mouse]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.setNavigationPinned(false);
    updateGeometry(fixture);
    fixture.hitRegions.libraryButtonBox = {.x_min = 3, .x_max = 22, .y_min = 20, .y_max = 20};
    auto const button = fixture.hitRegions.libraryButtonBox;
    CHECK(fixture.hitRegions.hitTestButton(4, 20).hoveredButton == HoveredButton::Library);
    REQUIRE(events.tryHandleEvent(clickBox(button)));
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(fixture.layoutCheckpointCount == 0);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('L')));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.shell.isNavigationPinned());
    fixture.hitRegions.clearFrameLocalRows();
    CHECK(fixture.hitRegions.libraryButtonBox.IsEmpty());
    CHECK(fixture.hitRegions.hitTestButton(4, 20).hoveredButton == HoveredButton::None);
  }

  TEST_CASE("PinnedNavigation - chooser search owns uppercase text and pinning closes the chooser",
            "[tui][regression][navigation][search]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addList("Lovely list");
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.setNavigationPinned(false);
    updateGeometry(fixture);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('l')));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('/')));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('L')));
    CHECK(fixture.shell.listSearch().query() == "L");
    CHECK_FALSE(fixture.shell.isNavigationPinned());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('L')));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.shell.isNavigationPinned());
    CHECK(fixture.shell.isNavigationFocused());
  }

  TEST_CASE("PinnedNavigation - narrow access uses the chooser without changing the pin preference",
            "[tui][regression][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.hitRegions.navigationLayout = navigationGeometry(80, 0, true);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('l')));
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.isNavigationPinned());
    CHECK(fixture.layoutCheckpointCount == 0);
  }

  TEST_CASE("PinnedNavigation - restoring room for a pinned tree retires its temporary chooser",
            "[tui][regression][navigation]")
  {
    auto shell = ShellInteractionModel{};
    shell.toggleNavigation(false);
    REQUIRE(shell.overlay() == Overlay::ListChooser);
    shell.reconcileNavigationLayout(true);
    CHECK(shell.overlay() == Overlay::None);
    CHECK(shell.isNavigationFocused());
    CHECK(shell.isNavigationPinned());
    shell.reconcileNavigationLayout(false);
    CHECK_FALSE(shell.isNavigationFocused());
    shell.setNavigationPinned(false);
    shell.toggleNavigation(false);
    shell.reconcileNavigationLayout(true);
    CHECK(shell.overlay() == Overlay::ListChooser);
    CHECK_FALSE(shell.isNavigationFocused());
    CHECK_FALSE(shell.isNavigationPinned());
  }

  TEST_CASE("PinnedNavigation - footer search starts local input and disappears outside its context",
            "[tui][regression][navigation][search]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    library.setFilterDraft("unchanged track filter");
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    keymap.applyOverrides({{"tui.library.openQuickFilter", {"F4"}}});
    auto const plan = KeymapPlan{keymap};
    auto events = fixture.makeEvents(library, plan);
    fixture.shell.focusNavigation();
    updateGeometry(fixture);
    auto renderStatus = [&]
    {
      return renderElement(statusBar(ao::test::englishMessageCatalog(),
                                     {.terminalColumns = 140,
                                      .navigationSearching = library.navigation().search().isActive(),
                                      .shell = &fixture.shell,
                                      .navigationSearchBox = &fixture.hitRegions.navigationSearchBox},
                                     plan),
                           140,
                           1);
    };
    auto const idle = renderStatus();
    CHECK(idle.text.contains("/ search"));
    REQUIRE_FALSE(fixture.hitRegions.navigationSearchBox.IsEmpty());
    fixture.hitRegions.navigationPinBox = {.x_min = 25, .x_max = 25, .y_min = 10, .y_max = 10};
    REQUIRE(events.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::None, .motion = ftxui::Mouse::Moved, .x = 25, .y = 10})));
    CHECK(events.hoveredButton() == HoveredButton::NavigationToggle);
    auto const searchBox = fixture.hitRegions.navigationSearchBox;
    REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse(
      "", {.button = ftxui::Mouse::None, .motion = ftxui::Mouse::Moved, .x = searchBox.x_min, .y = searchBox.y_min})));
    CHECK(events.hoveredButton() == HoveredButton::None);
    CHECK(fixture.shell.isNavigationFocused());
    REQUIRE(events.tryHandleEvent(clickBox(searchBox)));
    CHECK(library.navigation().search().isActive());
    CHECK_FALSE(fixture.shell.isInputActive());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character("All")));
    CHECK(library.navigation().search().query() == "All");
    renderStatus();
    CHECK(fixture.hitRegions.navigationSearchBox.IsEmpty());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(library.navigation().search().isActive());
    CHECK(fixture.shell.isNavigationFocused());
    CHECK(library.filterDraft() == "unchanged track filter");
    renderStatus();
    CHECK_FALSE(fixture.hitRegions.navigationSearchBox.IsEmpty());
    fixture.shell.focusTracks();
    renderStatus();
    CHECK(fixture.hitRegions.navigationSearchBox.IsEmpty());
    fixture.shell.focusNavigation();
    fixture.shell.openOverlay(Overlay::Help);
    renderStatus();
    CHECK(fixture.hitRegions.navigationSearchBox.IsEmpty());
  }

  TEST_CASE("PinnedNavigation - sidebar arrow hover follows the clickable target", "[tui][regression][mouse]")
  {
    using namespace ftxui;
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.hitRegions.navigationPinBox = {.x_min = 25, .x_max = 25, .y_min = 10, .y_max = 10};
    fixture.hitRegions.detailToggleBox = {.x_min = 79, .x_max = 79, .y_min = 10, .y_max = 10};
    auto move = [&](std::int32_t column, std::int32_t row)
    {
      return controller.tryHandleEvent(
        Event::Mouse("", {.button = Mouse::None, .motion = Mouse::Moved, .x = column, .y = row}));
    };
    REQUIRE(move(25, 10));
    CHECK(controller.hoveredButton() == HoveredButton::NavigationToggle);
    REQUIRE(move(79, 10));
    CHECK(controller.hoveredButton() == HoveredButton::DetailToggle);
    REQUIRE(move(79, 9));
    CHECK(controller.hoveredButton() == HoveredButton::None);
    REQUIRE(move(79, 10));
    REQUIRE(controller.tryHandleEvent(clickBox(fixture.hitRegions.detailToggleBox)));
    CHECK(fixture.shell.isDetailVisible());
    CHECK(controller.hoveredButton() == HoveredButton::None);
    fixture.hitRegions.detailToggleBox = {.x_min = 42, .x_max = 42, .y_min = 10, .y_max = 10};
    REQUIRE(move(42, 10));
    CHECK(controller.hoveredButton() == HoveredButton::DetailToggle);
    REQUIRE(controller.tryHandleEvent(Event::Character("?")));
    CHECK(controller.hoveredButton() == HoveredButton::None);
    move(42, 10);
    CHECK(controller.hoveredButton() == HoveredButton::None);
  }

  TEST_CASE("PinnedNavigation - detail edge toggles independently and cannot act behind a modal",
            "[tui][regression][detail][mouse]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    auto const edge = ftxui::Box{.x_min = 79, .x_max = 79, .y_min = 10, .y_max = 10};
    fixture.hitRegions.detailToggleBox = edge;
    REQUIRE(controller.tryHandleEvent(clickBox(edge)));
    CHECK(fixture.shell.isDetailVisible());
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.hitRegions.detailToggleBox.IsEmpty());
    fixture.hitRegions.detailToggleBox = edge;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    CHECK_FALSE(fixture.shell.isDetailVisible());
    CHECK(fixture.hitRegions.detailToggleBox.IsEmpty());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    fixture.shell.openOverlay(Overlay::Help);
    fixture.hitRegions.overlayPanel.box = {.x_min = 20, .x_max = 60, .y_min = 2, .y_max = 15};
    fixture.hitRegions.detailToggleBox = edge;
    REQUIRE(controller.tryHandleEvent(clickBox(edge)));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.shell.isDetailVisible());
    fixture.hitRegions.detailToggleBox = edge;
    REQUIRE(controller.tryHandleEvent(clickBox(edge)));
    CHECK_FALSE(fixture.shell.isDetailVisible());
    fixture.hitRegions.detailToggleBox = edge;
    fixture.hitRegions.clearFrameLocalRows();
    CHECK(fixture.hitRegions.detailToggleBox.IsEmpty());
    CHECK(fixture.hitRegions.detailPanel.box.IsEmpty());
  }

  TEST_CASE("PinnedNavigation - pinning resolves focus using the resulting constrained detail width",
            "[tui][regression][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.setNavigationPinned(false);
    fixture.shell.toggleDetail();
    fixture.shell.setPanelWidths(PanelWidths{.detail = 45});
    fixture.hitRegions.navigationLayout = navigationGeometry(140, 45, false, false, fixture.shell.panelWidths());
    REQUIRE_FALSE(fixture.hitRegions.navigationLayout.canDock);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('L')));
    CHECK(fixture.shell.isNavigationFocused());
    fixture.hitRegions.navigationLayout = navigationGeometry(140, 45, true, false, fixture.shell.panelWidths());
    REQUIRE(fixture.hitRegions.navigationLayout.docked);
    fixture.shell.reconcileNavigationLayout(fixture.hitRegions.navigationLayout.canDock);
    CHECK(fixture.shell.isNavigationFocused());

    fixture.shell.setNavigationPinned(false);
    fixture.hitRegions.navigationLayout = navigationGeometry(60, 45, false, false, fixture.shell.panelWidths());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('L')));
    CHECK(fixture.shell.isNavigationPinned());
    CHECK(fixture.shell.isTracksFocused());
  }

  TEST_CASE("PinnedNavigation - docked List disclosure focuses navigation and expands without opening a List",
            "[tui][regression][mouse][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto const parentId = fixture.addList("Parent");
    REQUIRE(rt::test::runRuntimeTask(
      *fixture.runtimePtr,
      fixture.runtimePtr->library().commands().createListAsync(rt::ListDraft{.parentId = parentId, .name = "Child"})));
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.hitRegions.navigationLayout = navigationGeometry(120, 0, true);

    auto const currentList = library.currentListId();
    auto const activeView = library.activeViewId();
    auto const cursor = library.navigation().cursor();
    auto const parentBefore = std::ranges::find(library.navigation().rows(), parentId, &ListNavigationRow::id);
    REQUIRE(parentBefore != library.navigation().rows().end());
    REQUIRE(parentBefore->hasChildren);
    REQUIRE_FALSE(parentBefore->expanded);

    auto const rendered = renderElement(navigationPanel(ao::test::englishMessageCatalog(),
                                                        library.navigation(),
                                                        library.currentListId(),
                                                        {.columns = 26, .regions = &fixture.hitRegions.navigation}),
                                        26,
                                        15);
    INFO(rendered.text);
    auto const hit = std::ranges::find(fixture.hitRegions.navigation.rows, parentId, &NavigationRowHit::id);
    REQUIRE(hit != fixture.hitRegions.navigation.rows.end());
    auto const disclosure = hit->disclosure;
    REQUIRE_FALSE(disclosure.IsEmpty());

    REQUIRE(controller.tryHandleEvent(clickBox(disclosure)));

    auto const parentAfter = std::ranges::find(library.navigation().rows(), parentId, &ListNavigationRow::id);
    REQUIRE(parentAfter != library.navigation().rows().end());
    CHECK(parentAfter->expanded);
    CHECK(fixture.shell.isNavigationFocused());
    CHECK(library.navigation().cursor() == cursor);
    CHECK(library.currentListId() == currentList);
    CHECK(library.activeViewId() == activeView);
    CHECK(fixture.layoutCheckpointCount == 0);
  }

  TEST_CASE("PinnedNavigation - a foreground interaction cancels scrollbar capture",
            "[tui][regression][navigation][mouse]")
  {
    auto fixture = EventControllerFixture{};

    for (std::int32_t index = 0; index < 12; ++index)
    {
      fixture.addList("List " + std::to_string(index));
    }

    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    updateGeometry(fixture);
    std::ignore = renderElement(navigationPanel(ao::test::englishMessageCatalog(),
                                                library.navigation(),
                                                library.currentListId(),
                                                {.columns = 26, .regions = &fixture.hitRegions.navigation}),
                                26,
                                8);
    auto const viewport = fixture.hitRegions.navigation.panel.navigationBox;
    REQUIRE(viewport.y_min < viewport.y_max);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse(
      "", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = viewport.x_max, .y = viewport.y_max})));
    auto const cursor = library.navigation().cursor();
    REQUIRE(library.navigation().selectedIndex() > 0);

    SECTION("Help opens and closes without mouse motion")
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character('?')));
      REQUIRE(fixture.shell.overlay() == Overlay::Help);
      REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    }

    SECTION("Command input opens and closes without mouse motion")
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character(':')));
      REQUIRE(fixture.shell.isInputActive());
      REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    }

    REQUIRE_FALSE(fixture.shell.isInputActive());
    REQUIRE(fixture.shell.overlay() == Overlay::None);
    CHECK(library.navigation().cursor() == cursor);
    std::ignore = events.tryHandleEvent(ftxui::Event::Mouse(
      "", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = viewport.x_max, .y = viewport.y_min}));
    CHECK(library.navigation().cursor() == cursor);
  }
} // namespace ao::tui::test
