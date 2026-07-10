// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Render.h"

#include "test/unit/tui/TuiRenderTestSupport.h"
#include "tui/CommandPalettePanel.h"
#include "tui/NotificationCenterPanel.h"
#include "tui/PresentationPanel.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include "tui/Style.h"
#include "tui/TrackListEntry.h"
#include "tui/TrackPresentationNavigation.h"
#include "tui/TuiHitRegions.h"
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("Render - help pane advertises workspace commands", "[tui][unit][render]")
  {
    auto const text = renderText(helpPane());

    CHECK(text.contains("/current"));
    CHECK(text.contains("/view <id>"));
    CHECK(text.contains("/output"));
    CHECK(text.contains("/views"));
    CHECK(text.contains("/notifications"));
    CHECK(text.contains("{ / }"));
  }

  TEST_CASE("Render - side panes size to content and terminal bounds", "[tui][unit][render]")
  {
    auto row =
      rt::TrackRow{.id = TrackId{7}, .title = "A very long title that should widen detail content", .artist = "Artist"};
    auto item = makeTrackListEntry(row);

    CHECK(helpPaneColumns(120) > 0);
    CHECK(helpPaneColumns(30) == 30);
    CHECK(detailPaneColumns(&item, 120) > detailPaneColumns(nullptr, 120));
    CHECK(detailPaneColumns(&item, 40) == 40);
  }

  TEST_CASE("Render - center popover places content in the screen middle", "[tui][unit][render]")
  {
    auto const rendered =
      renderElement(centerPopover(ftxui::text("Popup") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 5)), 21, 7);

    auto const optPopupBox = findTextCells(rendered.screen, "Popup");

    REQUIRE(optPopupBox);
    CHECK(optPopupBox->x_min == 8);
    CHECK(optPopupBox->y_min == 3);
  }

  TEST_CASE("Render - center popover clears a one-cell halo", "[tui][unit][render]")
  {
    auto rows = ftxui::Elements{};

    for (std::int32_t row = 0; row < 7; ++row)
    {
      rows.push_back(ftxui::text(std::string(21, '#')));
    }

    auto const rendered =
      renderElement(ftxui::dbox({
                      ftxui::vbox(std::move(rows)),
                      centerPopover(ftxui::text("Popup") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 5)),
                    }),
                    21,
                    7);

    CHECK(rendered.screen.PixelAt(7, 3).character == " ");
    CHECK(rendered.screen.PixelAt(13, 3).character == " ");
    CHECK(rendered.screen.PixelAt(8, 2).character == " ");
    CHECK(rendered.screen.PixelAt(8, 4).character == " ");
    CHECK(rendered.screen.PixelAt(6, 3).character == "#");
    CHECK(rendered.screen.PixelAt(14, 3).character == "#");
  }

  TEST_CASE("Render - titled panel places workspace context on the lower edge", "[tui][unit][render]")
  {
    auto listBox = ftxui::Box{};
    auto viewBox = ftxui::Box{};
    auto const rendered =
      renderElement(style::titledPanel(
                      "",
                      ftxui::text("Body"),
                      style::PanelOptions{
                        .leftFooter = style::PanelEdgeButton{.label = "list", .value = "All Tracks", .box = &listBox},
                        .leftFooterRight = style::PanelEdgeButton{.label = "view", .value = "albums", .box = &viewBox},
                        .rightFooter = "3 / 8 tracks"}),
                    64,
                    5);

    auto const optListLabelBox = findTextCells(rendered.screen, "list");
    auto const optListValueBox = findTextCells(rendered.screen, "All Tracks");
    auto const optViewLabelBox = findTextCells(rendered.screen, "view");
    auto const optViewValueBox = findTextCells(rendered.screen, "albums");
    auto const optFooterBox = findTextCells(rendered.screen, "3 / 8 tracks");
    REQUIRE(optListLabelBox);
    REQUIRE(optListValueBox);
    REQUIRE(optViewLabelBox);
    REQUIRE(optViewValueBox);
    REQUIRE(optFooterBox);
    CHECK_FALSE(rendered.text.contains("view:"));
    CHECK(optListLabelBox->y_min == 4);
    CHECK(optListValueBox->y_min == 4);
    CHECK(optViewLabelBox->y_min == 4);
    CHECK(optViewValueBox->y_min == 4);
    CHECK(listBox.y_min == 4);
    CHECK(listBox.x_min <= optListLabelBox->x_min);
    CHECK(listBox.x_max >= optListValueBox->x_max);
    CHECK(viewBox.y_min == 4);
    CHECK(viewBox.x_min <= optViewLabelBox->x_min);
    CHECK(viewBox.x_max >= optViewValueBox->x_max);
    CHECK(rendered.screen.PixelAt(1, 0).character == "─");
    CHECK(rendered.screen.PixelAt(0, 4).character == "╰");
    CHECK(rendered.screen.PixelAt(1, 4).character == "─");
    auto const checkFrameEdgePixel = [&](std::int32_t const column)
    {
      auto const pixel = rendered.screen.PixelAt(column, 4);
      CHECK(pixel.foreground_color == ftxui::Color::Default);
      CHECK_FALSE(pixel.bold);
      CHECK_FALSE(pixel.dim);
    };
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 1, 4).character == " ");
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 2, 4).character == "─");
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 3, 4).character == " ");
    checkFrameEdgePixel(optListValueBox->x_max + 1);
    checkFrameEdgePixel(optListValueBox->x_max + 2);
    checkFrameEdgePixel(optListValueBox->x_max + 3);
    CHECK(rendered.screen.PixelAt(optListLabelBox->x_min, 4).dim);
    CHECK(rendered.screen.PixelAt(optViewLabelBox->x_min, 4).dim);
    CHECK(rendered.screen.PixelAt(optListValueBox->x_min, 4).foreground_color == ftxui::Color::Cyan);
    CHECK(rendered.screen.PixelAt(optViewValueBox->x_min, 4).foreground_color == ftxui::Color::Cyan);
    CHECK(optFooterBox->y_min == 4);
    CHECK(rendered.screen.PixelAt(optFooterBox->x_min - 2, 4).character == "─");
    checkFrameEdgePixel(optFooterBox->x_min - 2);
    checkFrameEdgePixel(optFooterBox->x_min - 1);
    CHECK(rendered.screen.PixelAt(62, 4).character == "─");
    CHECK(rendered.screen.PixelAt(63, 4).character == "╯");
  }

  TEST_CASE("Render - hovered edge button uses interactive surface without tinting frame separators",
            "[tui][unit][render]")
  {
    auto const rendered =
      renderElement(style::titledPanel(
                      "",
                      ftxui::text("Body"),
                      style::PanelOptions{
                        .leftFooter = style::PanelEdgeButton{.label = "list", .value = "All Tracks", .hovered = true},
                        .leftFooterRight = style::PanelEdgeButton{.label = "view", .value = "albums"}}),
                    64,
                    5);

    auto const optListLabelBox = findTextCells(rendered.screen, "list");
    auto const optListValueBox = findTextCells(rendered.screen, "All Tracks");
    auto const optViewValueBox = findTextCells(rendered.screen, "albums");
    REQUIRE(optListLabelBox);
    REQUIRE(optListValueBox);
    REQUIRE(optViewValueBox);

    auto const hoveredPixel = rendered.screen.PixelAt(optListLabelBox->x_min, optListLabelBox->y_min);
    checkInteractiveSurface(hoveredPixel);
    CHECK(rendered.screen.PixelAt(optListValueBox->x_min, optListValueBox->y_min).background_color ==
          ftxui::Color::Yellow);
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 2, optListValueBox->y_min).foreground_color ==
          ftxui::Color::Default);
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 2, optListValueBox->y_min).background_color ==
          ftxui::Color::Default);
    CHECK(rendered.screen.PixelAt(optViewValueBox->x_min, optViewValueBox->y_min).foreground_color ==
          ftxui::Color::Cyan);
    CHECK(rendered.screen.PixelAt(optViewValueBox->x_min, optViewValueBox->y_min).background_color ==
          ftxui::Color::Default);
  }

  TEST_CASE("Render - titled panel body padding is opt-in for popovers", "[tui][unit][render]")
  {
    auto const unpadded = renderElement(style::titledPanel("Panel", ftxui::text("Body")), 16, 3);
    auto const padded = renderElement(style::popupPanel("Panel", ftxui::text("Body")), 16, 3);

    auto const optUnpaddedBody = findTextCells(unpadded.screen, "Body");
    auto const optPaddedBody = findTextCells(padded.screen, "Body");

    REQUIRE(optUnpaddedBody);
    REQUIRE(optPaddedBody);
    CHECK(optUnpaddedBody->x_min == 1);
    CHECK(optPaddedBody->x_min == 2);
  }

  TEST_CASE("Render - wide idle status bar reserves empty activity space without slot chrome", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto const rendered = renderElement(statusBar(StatusBarViewState{.shell = &shell}), 140, 1);

    CHECK_FALSE(rendered.text.contains("Ready"));
    CHECK_FALSE(rendered.text.contains("3 / 8 tracks"));
    CHECK(lineIndexContaining(rendered.text, "/ command") == 0);
    CHECK(rendered.text.contains("/ command"));
    CHECK(rendered.text.contains("l lists"));
    CHECK(rendered.text.contains("v view"));
    CHECK(rendered.text.contains("n notif"));
    CHECK(rendered.text.contains("d detail"));
    CHECK(rendered.text.contains("a pipeline"));
    CHECK(rendered.text.contains("o output"));
    CHECK(rendered.text.contains("Ctrl-L current"));
    CHECK(rendered.text.contains("q quit"));
    CHECK_FALSE(rendered.text.contains("Mode:"));
    CHECK_FALSE(rendered.text.contains("Filter:"));
    CHECK_FALSE(rendered.text.contains("view:"));

    auto const optShortcutBox = findTextCells(rendered.screen, "/ command");
    REQUIRE(optShortcutBox);
    CHECK(optShortcutBox->x_min > 0);
    CHECK_FALSE(rendered.text.contains("│"));

    auto const shortcutPixel = rendered.screen.PixelAt(optShortcutBox->x_min, optShortcutBox->y_min);
    CHECK(shortcutPixel.foreground_color == ftxui::Color::Cyan);
    CHECK(shortcutPixel.bold);
  }

  TEST_CASE("Render - narrow idle status bar collapses empty activity slot", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto const rendered = renderElement(statusBar(StatusBarViewState{.terminalColumns = 80, .shell = &shell}), 140, 2);

    CHECK_FALSE(rendered.text.contains("Ready"));
    CHECK_FALSE(rendered.text.contains("3 / 8 tracks"));
    CHECK(lineIndexContaining(rendered.text, "/ command") == 0);
    CHECK(rendered.text.contains("q quit"));
    CHECK(rendered.screen.PixelAt(0, 0).character == "/");
  }

  TEST_CASE("Render - status bar shows filter only when applied", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto const rendered = renderText(statusBar(StatusBarViewState{.filterDraft = "Aimer", .shell = &shell}));

    CHECK(rendered.contains("Filter: Aimer"));
    CHECK_FALSE(rendered.contains("Filter: -"));
  }

  TEST_CASE("Render - status bar uses overlay-specific help for every overlay", "[tui][unit][render]")
  {
    struct Case final
    {
      Overlay overlay = Overlay::None;
      std::string_view label{};
      std::string_view hint{};
    };

    auto const cases = std::vector<Case>{
      {.overlay = Overlay::ListChooser, .label = "Lists", .hint = "l toggle  Enter open  Esc close"},
      {.overlay = Overlay::DetailPanel, .label = "Detail", .hint = "d toggle  Esc close"},
      {.overlay = Overlay::QualityPanel, .label = "Pipeline", .hint = "a toggle  Esc close"},
      {.overlay = Overlay::OutputDevices, .label = "Output", .hint = "o toggle  Enter select  Esc close"},
      {.overlay = Overlay::PresentationPanel, .label = "Views", .hint = "v toggle  Enter select  Esc close"},
      {.overlay = Overlay::Notifications, .label = "Notifications", .hint = "n toggle  x hide compact  Esc close"},
      {.overlay = Overlay::Help, .label = "Help", .hint = "Esc close"},
    };

    for (auto const& item : cases)
    {
      auto shell = ShellInteractionModel{};
      shell.openOverlay(item.overlay);

      auto const rendered = renderText(statusBar(StatusBarViewState{.shell = &shell}));

      CHECK(rendered.contains(item.label));
      CHECK(rendered.contains(item.hint));
      CHECK_FALSE(rendered.contains("/ command"));
    }
  }

  TEST_CASE("Render - status slot shows activity compact state", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto activity = uimodel::ActivityStatusViewState{.compact = uimodel::ActivityCompactState{
                                                       .kind = uimodel::ActivityStatusKind::Warning,
                                                       .text = "Partial import",
                                                       .dismissible = true,
                                                       .hasDetails = true,
                                                     }};
    auto activityBox = ftxui::Box{};

    auto const rendered = renderElement(
      statusBar(StatusBarViewState{.activityStatus = &activity, .shell = &shell, .activityStatusBox = &activityBox}),
      180,
      1);

    CHECK(rendered.text.contains("warn"));
    CHECK(rendered.text.contains("Partial import"));
    CHECK_FALSE(rendered.text.contains("Ready"));
    CHECK(activityBox.x_min == 0);
    CHECK(activityBox.y_min == 0);
  }

  TEST_CASE("Render - idle status bar clears stale activity hit box", "[tui][regression][render]")
  {
    auto shell = ShellInteractionModel{};
    auto activity = uimodel::ActivityStatusViewState{.compact = uimodel::ActivityCompactState{
                                                       .kind = uimodel::ActivityStatusKind::Info,
                                                       .text = "Ready",
                                                       .dismissible = true,
                                                     }};
    auto activityBox = ftxui::Box{};

    renderElement(
      statusBar(StatusBarViewState{.activityStatus = &activity, .shell = &shell, .activityStatusBox = &activityBox}),
      180,
      1);
    REQUIRE(hasHitArea(activityBox));

    activity = uimodel::ActivityStatusViewState{};
    renderElement(
      statusBar(StatusBarViewState{.activityStatus = &activity, .shell = &shell, .activityStatusBox = &activityBox}),
      180,
      1);

    CHECK_FALSE(hasHitArea(activityBox));
  }

  TEST_CASE("Render - hovered status slot uses a readable interactive surface", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto activity = uimodel::ActivityStatusViewState{.compact = uimodel::ActivityCompactState{
                                                       .kind = uimodel::ActivityStatusKind::Warning,
                                                       .text = "Partial import",
                                                       .dismissible = true,
                                                       .hasDetails = true,
                                                     }};

    auto const rendered = renderElement(
      statusBar(StatusBarViewState{.activityStatus = &activity, .shell = &shell, .activityStatusHovered = true}),
      180,
      1);

    auto const optWarnBox = findTextCells(rendered.screen, "warn");
    REQUIRE(optWarnBox);
    auto const pixel = rendered.screen.PixelAt(optWarnBox->x_min, optWarnBox->y_min);
    checkInteractiveSurface(pixel);
  }

  TEST_CASE("Render - notification center lists clearable notification details", "[tui][unit][render]")
  {
    auto state = uimodel::ActivityStatusViewState{
      .compact =
        uimodel::ActivityCompactState{
          .kind = uimodel::ActivityStatusKind::Error,
          .text = "Scan failed",
          .persistent = true,
          .dismissible = true,
          .hasDetails = true,
        },
      .detail = uimodel::ActivityDetailState{
        .items = {uimodel::ActivityDetailItem{.id = rt::NotificationId{9},
                                              .severity = rt::NotificationSeverity::Error,
                                              .title = "Scan failed",
                                              .message = "Permission denied",
                                              .dismissible = true}},
      }};
    auto rowHitRegions = std::vector<NotificationDetailRowHitRegion>{};

    auto const rendered = renderElement(notificationCenterPanel(state, &rowHitRegions), 64, 12);

    CHECK(rendered.text.contains("Notifications"));
    CHECK(rendered.text.contains("Scan failed"));
    CHECK(rendered.text.contains("Permission denied"));
    CHECK(rendered.text.contains("click clearable row"));
    REQUIRE(rowHitRegions.size() == 1);
    CHECK(rowHitRegions.front().id == rt::NotificationId{9});
    CHECK(rowHitRegions.front().dismissible);
    CHECK(rowHitRegions.front().box.y_min > 0);
  }

  TEST_CASE("Render - command mode leaves bottom status bar in workspace layout", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginCommand("view albums");

    auto const rendered = renderText(statusBar(StatusBarViewState{.terminalColumns = 100, .shell = &shell}));

    CHECK_FALSE(rendered.contains("Command"));
    CHECK_FALSE(rendered.contains("/view albums"));
    CHECK_FALSE(rendered.contains("Tab complete"));
    CHECK(rendered.contains("/ command"));
  }

  TEST_CASE("Render - command palette keeps ratio-sized dimensions stable across completion content",
            "[tui][unit][render]")
  {
    auto const wideColumns = commandPalettePanelColumns(180);
    CHECK(commandPalettePanelColumns(0) == 72);
    CHECK(wideColumns == 72);
    CHECK(commandPalettePanelColumns(100) == 56);
    CHECK(commandPalettePanelColumns(40) == 40);
    CHECK(commandPalettePanelRows(0) == 18);
    CHECK(commandPalettePanelRows(30) == 12);
    CHECK(commandPalettePanelRows(50) == 18);
    CHECK(commandPalettePanelRows(80) == 20);
    CHECK(commandPalettePanelRows(8) == 8);

    auto shell = ShellInteractionModel{};
    shell.beginCommand("a");
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 1,
      .items = {rt::CompletionItem{.displayText = "a very long completion label that should not resize the panel",
                                   .insertText = "a very long completion label that should not resize the panel",
                                   .detail = "a long detail that should also stay inside the fixed ratio frame"}},
    });

    CHECK(commandPalettePanelColumns(180) == wideColumns);
  }

  TEST_CASE("Render - command palette keeps selected completion visible in constrained height",
            "[tui][regression][render]")
  {
    auto shell = ShellInteractionModel{};
    auto items = std::vector<rt::CompletionItem>{};

    for (std::int32_t index = 0; index < 8; ++index)
    {
      auto value = std::format("Option {}", index);
      items.push_back(rt::CompletionItem{.displayText = value, .insertText = value, .detail = "item"});
    }

    shell.beginCommand("o");
    shell.setCommandCompletion(rt::CompletionResult{.replaceBegin = 0, .replaceEnd = 1, .items = std::move(items)});

    for (std::int32_t index = 0; index < 7; ++index)
    {
      REQUIRE(shell.moveCommandCompletion(1));
    }

    auto const rendered =
      renderElement(commandPalettePanel(shell, 48) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 12), 48, 12);

    CHECK(rendered.text.contains("Option 7"));
    CHECK(rendered.text.contains("Tab complete"));
  }

  TEST_CASE("Render - command palette shows input and inline completion suffix", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginCommand("A");
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 1,
      .items = {rt::CompletionItem{.displayText = "Aimer", .insertText = "Aimer", .detail = "artist"}},
    });

    auto const rendered = renderText(commandPalettePanel(shell));

    CHECK(rendered.contains("Command Palette"));
    CHECK(rendered.contains("/A"));
    CHECK(rendered.contains("imer"));
    CHECK(rendered.contains("Aimer"));
    CHECK(rendered.contains("Tab complete"));
  }

  TEST_CASE("Render - command palette renders without completion matches", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginCommand("view albums");

    auto const rendered = renderText(commandPalettePanel(shell));

    CHECK(rendered.contains("/view albums"));
    CHECK(rendered.contains("No matches"));
    CHECK(rendered.contains("Enter run"));
  }

  TEST_CASE("Render - command palette keeps empty input separate from suggestions", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginCommand();
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 0,
      .items = {rt::CompletionItem{.displayText = "/output", .insertText = "output", .detail = "output device"}},
    });

    auto const rendered = renderText(commandPalettePanel(shell));

    CHECK(rendered.contains("/_"));
    CHECK(rendered.contains("/output"));
    CHECK(rendered.contains("Tab complete"));
  }

  TEST_CASE("Render - command palette renders completion metadata and fallback details", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginCommand();
    shell.setCommandCompletion(rt::CompletionResult{
      .items =
        {
          rt::CompletionItem{.displayText = "/view", .insertText = "view ", .detail = "track view"},
          rt::CompletionItem{.displayText = "Aimer", .insertText = "Aimer", .detail = "artist"},
        },
    });
    REQUIRE(shell.moveCommandCompletion(1));

    auto const rendered = renderElement(commandPalettePanel(shell, 48), 48, 8);

    CHECK(rendered.text.contains("Command Palette"));
    CHECK(rendered.text.contains("/view"));
    CHECK(rendered.text.contains("view"));
    CHECK(rendered.text.contains('v'));
    CHECK(rendered.text.contains("Aimer"));
    CHECK(rendered.text.contains("artist"));
    auto const optSelected = findTextCells(rendered.screen, "Aimer");
    REQUIRE(optSelected);
    CHECK_FALSE(rendered.screen.PixelAt(optSelected->x_min, optSelected->y_min).inverted);
    checkInteractiveSurface(rendered.screen.PixelAt(optSelected->x_min, optSelected->y_min));
  }

  TEST_CASE("Render - command palette does not infer metadata for non-command items", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginCommand();
    shell.setCommandCompletion(rt::CompletionResult{
      .items = {rt::CompletionItem{.displayText = "v", .insertText = "v", .detail = "artist"}},
    });

    auto const rendered = renderElement(commandPalettePanel(shell, 32), 32, 8);

    CHECK(rendered.text.contains('v'));
    CHECK(rendered.text.contains("artist"));
    CHECK_FALSE(rendered.text.contains("/v"));
    CHECK_FALSE(rendered.text.contains("view"));
  }

  TEST_CASE("Render - presentation panel renders selected and active views", "[tui][unit][render]")
  {
    auto const items = std::vector<TrackPresentationNavEntry>{
      {.id = "songs", .label = "Songs", .detail = "General-purpose song list."},
      {.id = "albums", .label = "Albums", .detail = "Grouped by album."},
    };
    auto rowHitRegions = std::vector<PresentationRowHitRegion>{};

    auto const rendered = renderElement(presentationPanel(items, "albums", 1, &rowHitRegions), 48, 16);

    CHECK(rendered.text.contains("Views"));
    CHECK(rendered.text.contains("albums"));
    CHECK(rendered.text.contains("* Albums"));
    CHECK(rendered.text.contains("Grouped by album."));
    CHECK_FALSE(rendered.text.contains("songs"));
    REQUIRE(rowHitRegions.size() == 2);
    CHECK(rowHitRegions[1].rowIndex == 1);
    CHECK_FALSE(rendered.screen.PixelAt(rowHitRegions[1].box.x_min, rowHitRegions[1].box.y_min).inverted);
    CHECK(rendered.screen.PixelAt(rowHitRegions[1].box.x_min, rowHitRegions[1].box.y_min).foreground_color ==
          ftxui::Color::Black);
    CHECK(rendered.screen.PixelAt(rowHitRegions[1].box.x_min, rowHitRegions[1].box.y_min).background_color ==
          ftxui::Color::Yellow);
  }

  TEST_CASE("Render - presentation panel width follows content and terminal bounds", "[tui][unit][render]")
  {
    auto const items = std::vector<TrackPresentationNavEntry>{
      {.id = "wide",
       .label = "Wide View",
       .detail = "Detailed description long enough to widen the picker beyond the default."},
    };

    auto const wideColumns = presentationPanelColumns(items, "wide", 120);

    CHECK(wideColumns > kPresentationPanelColumns);
    CHECK(wideColumns <= 120);
    CHECK(presentationPanelColumns(items, "wide", 60) == 60);
    CHECK(presentationPanelColumns({TrackPresentationNavEntry{.id = "x", .label = "X"}}, "x", 120) <
          kPresentationPanelColumns);

    auto const rendered = renderElement(presentationPanel(items, "wide", 0, nullptr, wideColumns), wideColumns, 16);

    CHECK(rendered.text.contains("Detailed description"));
  }

  TEST_CASE("Render - presentation panel handles empty and out-of-range selection", "[tui][unit][render]")
  {
    auto rowHitRegions = std::vector<PresentationRowHitRegion>{};
    auto rendered = renderElement(presentationPanel({}, "", 99, &rowHitRegions), 48, 16);

    CHECK(rendered.text.contains("default"));
    CHECK(rendered.text.contains("No views available"));
    CHECK(rowHitRegions.empty());

    auto const items = std::vector<TrackPresentationNavEntry>{
      {.id = "songs", .label = "Songs", .detail = "General-purpose song list."},
    };
    rendered = renderElement(presentationPanel(items, "songs", 99, &rowHitRegions), 48, 16);

    REQUIRE(rowHitRegions.size() == 1);
    CHECK_FALSE(rendered.screen.PixelAt(rowHitRegions[0].box.x_min, rowHitRegions[0].box.y_min).inverted);
  }
} // namespace ao::tui::test
