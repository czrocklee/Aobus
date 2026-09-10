// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/PanelResize.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/EventControllerTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/EventController.h"
#include "tui/HitRegions.h"
#include "tui/Keymap.h"
#include "tui/LibraryController.h"
#include "tui/ListNavigationModel.h"
#include "tui/NavigationPanel.h"
#include "tui/PanelWidths.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace ao::tui::test
{
  namespace
  {
    void layout(EventControllerFixture& fixture,
                EventController const& events,
                bool const separate = false,
                std::int32_t const columns = 180)
    {
      auto& hits = fixture.hitRegions;
      hits.navigationLayout = navigationGeometry(columns,
                                                 fixture.shell.isDetailVisible() ? 40 : 0,
                                                 fixture.shell.isNavigationPinned(),
                                                 separate,
                                                 events.panelWidths());
      auto const& geometry = hits.navigationLayout;
      auto const left = geometry.columns - 1;
      auto const right = columns - geometry.detailColumns - (separate ? 1 : 0);
      hits.navigationDividerBox = {.x_min = left, .x_max = left + (separate ? 1 : 0), .y_min = 1, .y_max = 30};
      hits.navigationPinBox = {.x_min = left, .x_max = left, .y_min = 15, .y_max = 15};
      hits.detailDividerBox = {.x_min = right, .x_max = right + (separate ? 1 : 0), .y_min = 1, .y_max = 30};
      hits.detailToggleBox = {
        .x_min = right + (separate ? 1 : 0), .x_max = right + (separate ? 1 : 0), .y_min = 15, .y_max = 15};
      hits.trackTableBox = {.x_min = left + 2, .x_max = right - 2, .y_min = 2, .y_max = 29};
    }

    ftxui::Event pointer(ftxui::Mouse::Motion const motion, std::int32_t const x, std::int32_t const y = 8)
    {
      return ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = motion, .x = x, .y = y});
    }
  } // namespace

  TEST_CASE("PanelResize - keyboard moves the focused boundary in screen direction", "[tui][unit][panel-resize]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.toggleDetail();
    layout(fixture, events);
    auto const right = ftxui::Event::Special("\x1b[1;2C");
    auto const left = ftxui::Event::Special("\x1b[1;2D");
    events.tryHandleEvent(right);
    CHECK(fixture.shell.panelWidths() == PanelWidths{});
    fixture.shell.focusNavigation();
    REQUIRE(events.tryHandleEvent(right));
    CHECK(fixture.shell.panelWidths().navigation == 27);
    layout(fixture, events);
    REQUIRE(events.tryHandleEvent(left));
    CHECK(fixture.shell.panelWidths().navigation == 26);
    fixture.shell.focusDetail();
    layout(fixture, events);
    REQUIRE(events.tryHandleEvent(left));
    CHECK(fixture.shell.panelWidths().detail == 41);
    layout(fixture, events);
    REQUIRE(events.tryHandleEvent(right));
    CHECK(fixture.shell.panelWidths().detail == 40);
    CHECK(fixture.layoutCheckpointCount == 4);
    // Repeated key events can arrive before another frame paints hit geometry.
    REQUIRE(events.tryHandleEvent(right));
    REQUIRE(events.tryHandleEvent(right));
    REQUIRE(events.tryHandleEvent(right));
    CHECK(fixture.shell.panelWidths().detail == 37);
    CHECK(fixture.layoutCheckpointCount == 7);
    fixture.shell.openOverlay(Overlay::Help);
    events.tryHandleEvent(left);
    CHECK(fixture.layoutCheckpointCount == 7);
  }

  TEST_CASE("PanelResize - dragging survives reflow and commits only on release", "[tui][unit][panel-resize][mouse]")
  {
    for (bool const separate : {false, true})
    {
      for (bool const detail : {false, true})
      {
        auto fixture = EventControllerFixture{};
        auto library = fixture.makeLibrary();
        auto events = fixture.makeEvents(library);
        fixture.shell.toggleDetail();
        layout(fixture, events, separate);
        auto const start =
          detail ? fixture.hitRegions.detailDividerBox.x_min : fixture.hitRegions.navigationDividerBox.x_min;
        REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Pressed, start)));
        REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Moved, start + 8)));
        CHECK(fixture.shell.panelWidths() == PanelWidths{});
        CHECK(fixture.layoutCheckpointCount == 0);
        CHECK(events.panelWidths().navigation == (detail ? 0 : 34));
        CHECK(events.panelWidths().detail == (detail ? 32 : 0));
        layout(fixture, events, separate);
        events.syncWorkspaceGeometry();
        CHECK(events.isPanelResizing(detail ? HoveredButton::DetailToggle : HoveredButton::NavigationToggle));
        REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Released, start + 10, 35)));
        CHECK(fixture.shell.panelWidths().navigation == (detail ? 0 : 36));
        CHECK(fixture.shell.panelWidths().detail == (detail ? 30 : 0));
        CHECK(fixture.layoutCheckpointCount == 1);
        CHECK(events.hoveredButton() == HoveredButton::None);
      }
    }
  }

  TEST_CASE("PanelResize - escape and terminal changes cancel an unfinished drag", "[tui][regression][panel-resize]")
  {
    for (bool const resizeTerminal : {false, true})
    {
      auto fixture = EventControllerFixture{};
      auto library = fixture.makeLibrary();
      auto events = fixture.makeEvents(library);
      fixture.shell.toggleDetail();
      layout(fixture, events);
      auto const start = fixture.hitRegions.detailDividerBox.x_min;
      REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Pressed, start)));
      REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Moved, start - 10)));

      if (resizeTerminal)
      {
        layout(fixture, events, false, 150);
        events.syncWorkspaceGeometry();
      }
      else
      {
        REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
      }

      CHECK(events.panelWidths() == PanelWidths{});
      CHECK(fixture.layoutCheckpointCount == 0);
      CHECK_FALSE(events.isPanelResizing(HoveredButton::DetailToggle));
      CHECK(events.hoveredButton() == HoveredButton::None);
      CHECK(fixture.shell.isDetailVisible());
    }
  }

  TEST_CASE("PanelResize - cancelling moved pointer input retires drag hover", "[tui][regression][panel-resize]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.toggleDetail();
    layout(fixture, events);
    auto const start = fixture.hitRegions.navigationDividerBox.x_min;
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Pressed, start)));
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Moved, start + 10, 35)));
    REQUIRE(events.hoveredButton() == HoveredButton::NavigationToggle);

    SECTION("Track navigation acts after rollback")
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character('j')));
      CHECK(library.selectedTrack() == 1);
    }

    SECTION("Help opens after rollback")
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character('?')));
      CHECK(fixture.shell.overlay() == Overlay::Help);
    }

    SECTION("Quit acts after rollback")
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character('Q')));
      CHECK(fixture.exitRequestCount == 1);
    }

    SECTION("Escape only cancels the drag")
    {
      library.toggleVisualSelection();
      REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
      CHECK(library.isVisualSelectionActive());
      CHECK(fixture.shell.overlay() == Overlay::None);
    }

    SECTION("Wheel input cancels")
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse(
        "", {.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = start + 10, .y = 35})));
    }

    SECTION("A new press cancels")
    {
      REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Pressed, start + 10, 35)));
    }

    SECTION("Releasing another button cancels")
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse(
        "", {.button = ftxui::Mouse::Right, .motion = ftxui::Mouse::Released, .x = start + 10, .y = 35})));
    }

    CHECK_FALSE(events.isPanelResizing(HoveredButton::NavigationToggle));
    CHECK(events.panelWidths() == PanelWidths{});
    CHECK(fixture.layoutCheckpointCount == 0);
    CHECK(events.hoveredButton() == HoveredButton::None);
  }

  TEST_CASE("PanelResize - normal release recomputes hover from the painted pointer target",
            "[tui][regression][panel-resize]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.toggleDetail();
    layout(fixture, events);
    auto const start = fixture.hitRegions.navigationDividerBox.x_min;
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Pressed, start)));
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Moved, start + 8)));
    layout(fixture, events);
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Released, start + 8)));
    CHECK_FALSE(events.isPanelResizing(HoveredButton::NavigationToggle));
    CHECK(events.hoveredButton() == HoveredButton::NavigationToggle);
    CHECK(fixture.layoutCheckpointCount == 1);
  }

  TEST_CASE("PanelResize - limits preserve readable panes and preferred sizes recover", "[tui][unit][panel-resize]")
  {
    for (bool const separate : {false, true})
    {
      auto const preferred = PanelWidths{.navigation = 45, .detail = 60};
      auto const wide = navigationGeometry(220, 40, true, separate, preferred);
      CHECK(wide.columns == 45);
      CHECK(wide.detailColumns == 60);
      auto const narrow = navigationGeometry(140, 40, true, separate, preferred);
      CHECK(narrow.docked);
      CHECK(narrow.columns >= kMinimumNavigationColumns);
      CHECK(narrow.detailColumns >= kMinimumDetailColumns);
      CHECK(narrow.trackColumns >= kMinimumTrackColumns);
      CHECK(narrow.columns < wide.columns);
      CHECK(navigationGeometry(220, 40, true, separate, preferred) == wide);
      CHECK(navigationGeometry(220, 0, false, separate, preferred).trackColumns == 217);
    }
  }

  TEST_CASE("PanelResize - dragging clamps and clicking the indicator keeps its toggle behavior",
            "[tui][unit][panel-resize][mouse]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.toggleDetail();
    layout(fixture, events);
    auto const start = fixture.hitRegions.navigationDividerBox.x_min;
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Pressed, start)));
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Moved, -1000)));
    CHECK(events.panelWidths().navigation == kMinimumNavigationColumns);
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Moved, 1000)));
    layout(fixture, events);
    CHECK(fixture.hitRegions.navigationLayout.trackColumns == kMinimumTrackColumns);
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Released, 1000)));
    layout(fixture, events);
    auto const toggle = fixture.hitRegions.navigationPinBox;
    REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Pressed, toggle.x_min, toggle.y_min)));
    CHECK_FALSE(fixture.shell.isNavigationPinned());
    CHECK_FALSE(events.isPanelResizing(HoveredButton::NavigationToggle));
  }

  TEST_CASE("PanelResize - active list search owns modified arrows", "[tui][regression][panel-resize]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.focusNavigation();
    layout(fixture, events);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('/')));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Special("\x1b[1;2C")));
    CHECK(fixture.shell.panelWidths() == PanelWidths{});
    CHECK(fixture.layoutCheckpointCount == 0);
  }

  TEST_CASE("PanelResize - shrinking detail can restore pinned lists without cancelling the drag",
            "[tui][regression][panel-resize]")
  {
    for (bool const separate : {false, true})
    {
      auto fixture = EventControllerFixture{};
      auto library = fixture.makeLibrary();
      auto events = fixture.makeEvents(library);
      fixture.shell.toggleDetail();
      layout(fixture, events, separate, 125);
      REQUIRE_FALSE(fixture.hitRegions.navigationLayout.docked);
      auto const start = fixture.hitRegions.detailDividerBox.x_min;
      REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Pressed, start)));
      REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Moved, start + 16)));
      layout(fixture, events, separate, 125);
      REQUIRE(fixture.hitRegions.navigationLayout.docked);
      events.syncWorkspaceGeometry();
      CHECK(events.isPanelResizing(HoveredButton::DetailToggle));
      REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Released, start + 16)));
      CHECK(fixture.shell.panelWidths().detail == kMinimumDetailColumns);
      CHECK(fixture.layoutCheckpointCount == 1);
    }
  }

  TEST_CASE("PanelResize - newly opened detail waits for its first painted width", "[tui][regression][panel-resize]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    layout(fixture, events);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('D')));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Special("\x1b[1;2D")));
    REQUIRE(events.tryHandleEvent(ftxui::Event::CtrlW));
    CHECK_FALSE(events.keyboardResizeDivider());
    CHECK(fixture.shell.panelWidths() == PanelWidths{});
    CHECK(fixture.layoutCheckpointCount == 0);
    layout(fixture, events);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Special("\x1b[1;2D")));
    CHECK(fixture.shell.panelWidths().detail == 41);
    REQUIRE(events.tryHandleEvent(ftxui::Event::CtrlW));
    CHECK(events.keyboardResizeDivider() == PanelDivider::Detail);
  }

  TEST_CASE("PanelResize - mode starts at the focused divider without taking workspace focus",
            "[tui][unit][panel-resize]")
  {
    for (auto const focus : {WorkspaceFocus::Tracks, WorkspaceFocus::Lists, WorkspaceFocus::Detail})
    {
      auto fixture = EventControllerFixture{};
      auto library = fixture.makeLibrary();
      auto events = fixture.makeEvents(library);
      fixture.shell.toggleDetail();

      if (focus == WorkspaceFocus::Lists)
      {
        fixture.shell.focusNavigation();
      }
      else if (focus == WorkspaceFocus::Detail)
      {
        fixture.shell.focusDetail();
      }

      layout(fixture, events);
      REQUIRE(events.tryHandleEvent(ftxui::Event::CtrlW));
      auto const expected = focus == WorkspaceFocus::Detail ? PanelDivider::Detail : PanelDivider::Navigation;
      CHECK(events.keyboardResizeDivider() == expected);
      CHECK(fixture.shell.isTracksFocused() == (focus == WorkspaceFocus::Tracks));
      CHECK(fixture.shell.isNavigationFocused() == (focus == WorkspaceFocus::Lists));
      CHECK(fixture.shell.isDetailFocused() == (focus == WorkspaceFocus::Detail));
      REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
      CHECK_FALSE(events.keyboardResizeDivider());
      CHECK(fixture.layoutCheckpointCount == 0);
    }
  }

  TEST_CASE("PanelResize - mode previews both boundaries and applies or cancels them together",
            "[tui][regression][panel-resize]")
  {
    for (bool const apply : {false, true})
    {
      auto fixture = EventControllerFixture{};
      auto library = fixture.makeLibrary();
      auto events = fixture.makeEvents(library);
      fixture.shell.toggleDetail();
      layout(fixture, events);
      REQUIRE(events.tryHandleEvent(ftxui::Event::CtrlW));
      REQUIRE(events.tryHandleEvent(ftxui::Event::ArrowRight));
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character('l')));
      CHECK(events.panelWidths().navigation == 28);
      REQUIRE(events.tryHandleEvent(ftxui::Event::Tab));
      CHECK(events.keyboardResizeDivider() == PanelDivider::Detail);
      REQUIRE(events.tryHandleEvent(ftxui::Event::ArrowLeft));
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character('h')));
      CHECK(events.panelWidths().detail == 42);
      CHECK(fixture.shell.panelWidths() == PanelWidths{});
      CHECK(fixture.layoutCheckpointCount == 0);
      layout(fixture, events);
      events.syncWorkspaceGeometry();
      CHECK(events.isPanelResizing(HoveredButton::DetailToggle));
      REQUIRE(events.tryHandleEvent(ftxui::Event::TabReverse));
      CHECK(events.keyboardResizeDivider() == PanelDivider::Navigation);
      REQUIRE(events.tryHandleEvent(apply ? ftxui::Event::Return : ftxui::Event::Escape));
      CHECK_FALSE(events.keyboardResizeDivider());
      CHECK(fixture.shell.panelWidths() == (apply ? PanelWidths{.navigation = 28, .detail = 42} : PanelWidths{}));
      CHECK(fixture.layoutCheckpointCount == (apply ? 1 : 0));
      CHECK(fixture.shell.isTracksFocused());
    }
  }

  TEST_CASE("PanelResize - mode skips hidden dividers and leaves a missing layout alone", "[tui][unit][panel-resize]")
  {
    for (bool const navigation : {false, true})
    {
      for (bool const detail : {false, true})
      {
        auto fixture = EventControllerFixture{};
        auto library = fixture.makeLibrary();
        auto events = fixture.makeEvents(library);
        fixture.shell.setNavigationPinned(navigation);

        if (detail)
        {
          fixture.shell.toggleDetail();
        }

        layout(fixture, events);
        REQUIRE(events.tryHandleEvent(ftxui::Event::CtrlW));
        CHECK(events.keyboardResizeDivider().has_value() == (navigation || detail));

        if (navigation || detail)
        {
          auto const expected = navigation ? PanelDivider::Navigation : PanelDivider::Detail;
          CHECK(events.keyboardResizeDivider() == expected);
          REQUIRE(events.tryHandleEvent(ftxui::Event::Tab));
          CHECK(events.keyboardResizeDivider() == (navigation && detail ? PanelDivider::Detail : expected));
          REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
        }

        CHECK(fixture.layoutCheckpointCount == 0);
      }
    }
  }

  TEST_CASE("PanelResize - text editing retains Ctrl W word deletion", "[tui][regression][panel-resize]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    layout(fixture, events);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character(':')));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character("first second")));
    REQUIRE(events.tryHandleEvent(ftxui::Event::CtrlW));
    CHECK(fixture.shell.inputDraft() == "first ");
    CHECK_FALSE(events.keyboardResizeDivider());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    fixture.shell.focusNavigation();
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('/')));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Character("first second")));
    REQUIRE(events.tryHandleEvent(ftxui::Event::CtrlW));
    CHECK(library.navigation().search().query() == "first ");
    CHECK_FALSE(events.keyboardResizeDivider());
  }

  TEST_CASE("PanelResize - mode entry can be rebound", "[tui][unit][panel-resize][keymap]")
  {
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    REQUIRE(keymap.tryUnbind("tui.workspace.beginPanelResize", *uimodel::KeyChord::parse("Ctrl+W")));
    REQUIRE(keymap.tryBind("tui.workspace.beginPanelResize", *uimodel::KeyChord::parse("F4")));
    auto const plan = KeymapPlan{keymap};
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library, plan);
    layout(fixture, events);
    REQUIRE(events.tryHandleEvent(ftxui::Event::F4));
    REQUIRE(events.tryHandleEvent(ftxui::Event::ArrowRight));
    REQUIRE(events.tryHandleEvent(ftxui::Event::F4));
    CHECK(fixture.shell.panelWidths().navigation == 27);
    CHECK_FALSE(events.keyboardResizeDivider());
    CHECK(plan.shortcutFor(KeyAction::BeginPanelResize) == "F4");
  }

  TEST_CASE("PanelResize - mode cancels on terminal changes and consumes a cancelling mouse press",
            "[tui][regression][panel-resize]")
  {
    for (bool const terminalChange : {false, true})
    {
      auto fixture = EventControllerFixture{};
      auto library = fixture.makeLibrary();
      auto events = fixture.makeEvents(library);
      layout(fixture, events);
      REQUIRE(events.tryHandleEvent(ftxui::Event::CtrlW));
      REQUIRE(events.tryHandleEvent(ftxui::Event::ArrowRight));

      if (terminalChange)
      {
        layout(fixture, events, false, 140);
        events.syncWorkspaceGeometry();
      }
      else
      {
        auto const toggle = fixture.hitRegions.navigationPinBox;
        REQUIRE(events.tryHandleEvent(pointer(ftxui::Mouse::Pressed, toggle.x_min, toggle.y_min)));
        CHECK(fixture.shell.isNavigationPinned());
      }

      CHECK_FALSE(events.keyboardResizeDivider());
      CHECK(events.panelWidths() == PanelWidths{});
      CHECK(fixture.layoutCheckpointCount == 0);
    }
  }

  TEST_CASE("PanelResize - shared screen deltas clamp safely and preserve untouched preferences",
            "[tui][unit][panel-resize]")
  {
    auto const widths = PanelWidths{.detail = 48};
    auto const geometry = navigationGeometry(180, 40, true, false, widths);
    auto const resize = PanelResize{widths, geometry, PanelDivider::Navigation};
    CHECK(resize.moved(0) == widths);
    CHECK(resize.moved(2) == PanelWidths{.navigation = 28, .detail = 48});
    CHECK(resize.moved(std::numeric_limits<std::int32_t>::min()).navigation == kMinimumNavigationColumns);
    auto const largest = resize.moved(std::numeric_limits<std::int32_t>::max());
    CHECK(navigationGeometry(180, 40, true, false, largest).trackColumns == kMinimumTrackColumns);
    CHECK(largest.detail == 48);
  }

  TEST_CASE("PanelResize - mode status identifies its divider and keeps activity and exit keys visible",
            "[tui][regression][panel-resize]")
  {
    auto fixture = EventControllerFixture{};
    fixture.runtimePtr->notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::pinned());

    for (auto const* locale : {"en", "zh-Hant", "fr"})
    {
      for (auto const divider : {PanelDivider::Navigation, PanelDivider::Detail})
      {
        for (auto const columns : {80, 140})
        {
          auto const rendered = renderElement(statusBar(ao::test::messageCatalog(locale),
                                                        {.activityStatus = &fixture.activityStatusViewModel.viewState(),
                                                         .terminalColumns = columns,
                                                         .optResizingDivider = divider,
                                                         .shell = &fixture.shell},
                                                        KeymapPlan{fixture.settingsKeymap}),
                                              columns,
                                              1);
          CAPTURE(locale, columns, rendered.text);
          CHECK(rendered.text.contains("Tab"));
          CHECK(rendered.text.contains("Enter"));
          CHECK(rendered.text.contains("Esc"));
          CHECK(rendered.text.contains("Partial import"));
          auto target = std::string{};

          if (std::string_view{locale} == "en")
          {
            target = divider == PanelDivider::Navigation ? "Lists" : "detail";
          }
          else if (std::string_view{locale} == "fr")
          {
            target = divider == PanelDivider::Navigation ? "Listes" : "détail";
          }
          else
          {
            target = divider == PanelDivider::Navigation ? "列表" : "詳情";
          }

          auto const optTargetBox = findTextCells(rendered.screen, target);
          auto const optActivityBox = findTextCells(rendered.screen, "Partial import");
          auto const optExitBox = findTextCells(rendered.screen, "Esc");
          REQUIRE(optTargetBox);
          REQUIRE(optActivityBox);
          REQUIRE(optExitBox);
          CHECK(optActivityBox->x_max < optTargetBox->x_min);
          CHECK(optTargetBox->x_max < optExitBox->x_min);
        }
      }
    }
  }
} // namespace ao::tui::test
