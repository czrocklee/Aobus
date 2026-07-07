// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Render.h"

#include "test/unit/tui/TuiRenderTestSupport.h"
#include "tui/CommandPalettePanel.h"
#include "tui/Model.h"
#include "tui/NotificationCenterPanel.h"
#include "tui/PresentationPanel.h"
#include "tui/ShellModel.h"
#include "tui/StatusBar.h"
#include "tui/Style.h"
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

    CHECK(text.find("/current") != std::string::npos);
    CHECK(text.find("/view <id>") != std::string::npos);
    CHECK(text.find("/output") != std::string::npos);
    CHECK(text.find("/views") != std::string::npos);
    CHECK(text.find("/notifications") != std::string::npos);
    CHECK(text.find("{ / }") != std::string::npos);
  }

  TEST_CASE("Render - side panes size to content and terminal bounds", "[tui][unit][render]")
  {
    auto row =
      rt::TrackRow{.id = TrackId{7}, .title = "A very long title that should widen detail content", .artist = "Artist"};
    auto item = makeTrackListItem(row);

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

    REQUIRE(optPopupBox.has_value());
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
    REQUIRE(optListLabelBox.has_value());
    REQUIRE(optListValueBox.has_value());
    REQUIRE(optViewLabelBox.has_value());
    REQUIRE(optViewValueBox.has_value());
    REQUIRE(optFooterBox.has_value());
    CHECK(rendered.text.find("view:") == std::string::npos);
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
    REQUIRE(optListLabelBox.has_value());
    REQUIRE(optListValueBox.has_value());
    REQUIRE(optViewValueBox.has_value());

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

    REQUIRE(optUnpaddedBody.has_value());
    REQUIRE(optPaddedBody.has_value());
    CHECK(optUnpaddedBody->x_min == 1);
    CHECK(optPaddedBody->x_min == 2);
  }

  TEST_CASE("Render - wide idle status bar reserves empty activity space without slot chrome", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    auto const rendered = renderElement(statusBar(StatusBarViewState{.shell = &shell}), 140, 1);

    CHECK(rendered.text.find("Ready") == std::string::npos);
    CHECK(rendered.text.find("3 / 8 tracks") == std::string::npos);
    CHECK(lineIndexContaining(rendered.text, "/ command") == 0);
    CHECK(rendered.text.find("/ command") != std::string::npos);
    CHECK(rendered.text.find("l lists") != std::string::npos);
    CHECK(rendered.text.find("v view") != std::string::npos);
    CHECK(rendered.text.find("n notif") != std::string::npos);
    CHECK(rendered.text.find("d detail") != std::string::npos);
    CHECK(rendered.text.find("a pipeline") != std::string::npos);
    CHECK(rendered.text.find("o output") != std::string::npos);
    CHECK(rendered.text.find("Ctrl-L current") != std::string::npos);
    CHECK(rendered.text.find("q quit") != std::string::npos);
    CHECK(rendered.text.find("Mode:") == std::string::npos);
    CHECK(rendered.text.find("Filter:") == std::string::npos);
    CHECK(rendered.text.find("view:") == std::string::npos);

    auto const optShortcutBox = findTextCells(rendered.screen, "/ command");
    REQUIRE(optShortcutBox.has_value());
    CHECK(optShortcutBox->x_min > 0);
    CHECK(rendered.text.find("│") == std::string::npos);

    auto const shortcutPixel = rendered.screen.PixelAt(optShortcutBox->x_min, optShortcutBox->y_min);
    CHECK(shortcutPixel.foreground_color == ftxui::Color::Cyan);
    CHECK(shortcutPixel.bold);
  }

  TEST_CASE("Render - narrow idle status bar collapses empty activity slot", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    auto const rendered = renderElement(statusBar(StatusBarViewState{.terminalColumns = 80, .shell = &shell}), 140, 2);

    CHECK(rendered.text.find("Ready") == std::string::npos);
    CHECK(rendered.text.find("3 / 8 tracks") == std::string::npos);
    CHECK(lineIndexContaining(rendered.text, "/ command") == 0);
    CHECK(rendered.text.find("q quit") != std::string::npos);
    CHECK(rendered.screen.PixelAt(0, 0).character == "/");
  }

  TEST_CASE("Render - status bar shows filter only when applied", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    auto const rendered = renderText(statusBar(StatusBarViewState{.filterDraft = "Aimer", .shell = &shell}));

    CHECK(rendered.find("Filter: Aimer") != std::string::npos);
    CHECK(rendered.find("Filter: -") == std::string::npos);
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
      auto shell = ShellModel{};
      shell.openOverlay(item.overlay);

      auto const rendered = renderText(statusBar(StatusBarViewState{.shell = &shell}));

      CHECK(rendered.find(item.label) != std::string::npos);
      CHECK(rendered.find(item.hint) != std::string::npos);
      CHECK(rendered.find("/ command") == std::string::npos);
    }
  }

  TEST_CASE("Render - status slot shows activity compact state", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
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

    CHECK(rendered.text.find("warn") != std::string::npos);
    CHECK(rendered.text.find("Partial import") != std::string::npos);
    CHECK(rendered.text.find("Ready") == std::string::npos);
    CHECK(activityBox.x_min == 0);
    CHECK(activityBox.y_min == 0);
  }

  TEST_CASE("Render - idle status bar clears stale activity hit box", "[tui][regression][render]")
  {
    auto shell = ShellModel{};
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
    auto shell = ShellModel{};
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
    REQUIRE(optWarnBox.has_value());
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
    auto rowBoxes = std::vector<NotificationDetailRowBox>{};

    auto const rendered = renderElement(notificationCenterPanel(state, &rowBoxes), 64, 12);

    CHECK(rendered.text.find("Notifications") != std::string::npos);
    CHECK(rendered.text.find("Scan failed") != std::string::npos);
    CHECK(rendered.text.find("Permission denied") != std::string::npos);
    CHECK(rendered.text.find("click clearable row") != std::string::npos);
    REQUIRE(rowBoxes.size() == 1);
    CHECK(rowBoxes.front().id == rt::NotificationId{9});
    CHECK(rowBoxes.front().dismissible);
    CHECK(rowBoxes.front().box.y_min > 0);
  }

  TEST_CASE("Render - command mode leaves bottom status bar in workspace layout", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    shell.beginCommand("view albums");

    auto const rendered = renderText(statusBar(StatusBarViewState{.terminalColumns = 100, .shell = &shell}));

    CHECK(rendered.find("Command") == std::string::npos);
    CHECK(rendered.find("/view albums") == std::string::npos);
    CHECK(rendered.find("Tab complete") == std::string::npos);
    CHECK(rendered.find("/ command") != std::string::npos);
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

    auto shell = ShellModel{};
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
    auto shell = ShellModel{};
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

    CHECK(rendered.text.find("Option 7") != std::string::npos);
    CHECK(rendered.text.find("Tab complete") != std::string::npos);
  }

  TEST_CASE("Render - command palette shows input and inline completion suffix", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    shell.beginCommand("A");
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 1,
      .items = {rt::CompletionItem{.displayText = "Aimer", .insertText = "Aimer", .detail = "artist"}},
    });

    auto const rendered = renderText(commandPalettePanel(shell));

    CHECK(rendered.find("Command Palette") != std::string::npos);
    CHECK(rendered.find("/A") != std::string::npos);
    CHECK(rendered.find("imer") != std::string::npos);
    CHECK(rendered.find("Aimer") != std::string::npos);
    CHECK(rendered.find("Tab complete") != std::string::npos);
  }

  TEST_CASE("Render - command palette renders without completion matches", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    shell.beginCommand("view albums");

    auto const rendered = renderText(commandPalettePanel(shell));

    CHECK(rendered.find("/view albums") != std::string::npos);
    CHECK(rendered.find("No matches") != std::string::npos);
    CHECK(rendered.find("Enter run") != std::string::npos);
  }

  TEST_CASE("Render - command palette keeps empty input separate from suggestions", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    shell.beginCommand();
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 0,
      .items = {rt::CompletionItem{.displayText = "/output", .insertText = "output", .detail = "output device"}},
    });

    auto const rendered = renderText(commandPalettePanel(shell));

    CHECK(rendered.find("/_") != std::string::npos);
    CHECK(rendered.find("/output") != std::string::npos);
    CHECK(rendered.find("Tab complete") != std::string::npos);
  }

  TEST_CASE("Render - command palette renders completion metadata and fallback details", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
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

    CHECK(rendered.text.find("Command Palette") != std::string::npos);
    CHECK(rendered.text.find("/view") != std::string::npos);
    CHECK(rendered.text.find("view") != std::string::npos);
    CHECK(rendered.text.find('v') != std::string::npos);
    CHECK(rendered.text.find("Aimer") != std::string::npos);
    CHECK(rendered.text.find("artist") != std::string::npos);
    auto const optSelected = findTextCells(rendered.screen, "Aimer");
    REQUIRE(optSelected.has_value());
    CHECK_FALSE(rendered.screen.PixelAt(optSelected->x_min, optSelected->y_min).inverted);
    checkInteractiveSurface(rendered.screen.PixelAt(optSelected->x_min, optSelected->y_min));
  }

  TEST_CASE("Render - command palette does not infer metadata for non-command items", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    shell.beginCommand();
    shell.setCommandCompletion(rt::CompletionResult{
      .items = {rt::CompletionItem{.displayText = "v", .insertText = "v", .detail = "artist"}},
    });

    auto const rendered = renderElement(commandPalettePanel(shell, 32), 32, 8);

    CHECK(rendered.text.find('v') != std::string::npos);
    CHECK(rendered.text.find("artist") != std::string::npos);
    CHECK(rendered.text.find("/v") == std::string::npos);
    CHECK(rendered.text.find("view") == std::string::npos);
  }

  TEST_CASE("Render - presentation panel renders selected and active views", "[tui][unit][render]")
  {
    auto const items = std::vector<PresentationNavItem>{
      {.id = "songs", .label = "Songs", .detail = "General-purpose song list."},
      {.id = "albums", .label = "Albums", .detail = "Grouped by album."},
    };
    auto rowBoxes = std::vector<PresentationRowBox>{};

    auto const rendered = renderElement(presentationPanel(items, "albums", 1, &rowBoxes), 48, 16);

    CHECK(rendered.text.find("Views") != std::string::npos);
    CHECK(rendered.text.find("albums") != std::string::npos);
    CHECK(rendered.text.find("* Albums") != std::string::npos);
    CHECK(rendered.text.find("Grouped by album.") != std::string::npos);
    CHECK(rendered.text.find("songs") == std::string::npos);
    REQUIRE(rowBoxes.size() == 2);
    CHECK(rowBoxes[1].rowIndex == 1);
    CHECK_FALSE(rendered.screen.PixelAt(rowBoxes[1].box.x_min, rowBoxes[1].box.y_min).inverted);
    CHECK(rendered.screen.PixelAt(rowBoxes[1].box.x_min, rowBoxes[1].box.y_min).foreground_color ==
          ftxui::Color::Black);
    CHECK(rendered.screen.PixelAt(rowBoxes[1].box.x_min, rowBoxes[1].box.y_min).background_color ==
          ftxui::Color::Yellow);
  }

  TEST_CASE("Render - presentation panel width follows content and terminal bounds", "[tui][unit][render]")
  {
    auto const items = std::vector<PresentationNavItem>{
      {.id = "wide",
       .label = "Wide View",
       .detail = "Detailed description long enough to widen the picker beyond the default."},
    };

    auto const wideColumns = presentationPanelColumns(items, "wide", 120);

    CHECK(wideColumns > kPresentationPanelColumns);
    CHECK(wideColumns <= 120);
    CHECK(presentationPanelColumns(items, "wide", 60) == 60);
    CHECK(presentationPanelColumns({PresentationNavItem{.id = "x", .label = "X"}}, "x", 120) <
          kPresentationPanelColumns);

    auto const rendered = renderElement(presentationPanel(items, "wide", 0, nullptr, wideColumns), wideColumns, 16);

    CHECK(rendered.text.find("Detailed description") != std::string::npos);
  }

  TEST_CASE("Render - presentation panel handles empty and out-of-range selection", "[tui][unit][render]")
  {
    auto rowBoxes = std::vector<PresentationRowBox>{};
    auto rendered = renderElement(presentationPanel({}, "", 99, &rowBoxes), 48, 16);

    CHECK(rendered.text.find("default") != std::string::npos);
    CHECK(rendered.text.find("No views available") != std::string::npos);
    CHECK(rowBoxes.empty());

    auto const items = std::vector<PresentationNavItem>{
      {.id = "songs", .label = "Songs", .detail = "General-purpose song list."},
    };
    rendered = renderElement(presentationPanel(items, "songs", 99, &rowBoxes), 48, 16);

    REQUIRE(rowBoxes.size() == 1);
    CHECK_FALSE(rendered.screen.PixelAt(rowBoxes[0].box.x_min, rowBoxes[0].box.y_min).inverted);
  }
} // namespace ao::tui::test
