// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Render.h"

#include "CoverArt.h"
#include "Model.h"
#include "Style.h"
#include "TextCell.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kCoverArtPanelColumns = 30;
    constexpr std::int32_t kCoverArtPanelRows = 16;
    constexpr std::int32_t kDetailPaneLabelColumns = 14;

    constexpr auto kHelpPaneLines = std::to_array<std::string_view>({
      "Commands",
      "/text              quick filter",
      "/lists or /l       choose list",
      "/detail or /d      show selected track detail",
      "/pipeline or /a    show audio pipeline",
      "/output or /o      choose output device",
      "/views or /v       choose presentation",
      "/notifications or /n show notification center",
      "/current           reveal current track",
      "/view <id>         switch presentation",
      "{ / }              previous / next group",
      "/clear             clear filter",
      "/reload            reload active list",
      "/play /pause /stop playback",
      "/quit              exit",
      "Esc close  Enter run",
    });
    constexpr std::size_t kHelpPaneTitleLine = 0;
    constexpr std::size_t kHelpPaneCommandFirstLine = kHelpPaneTitleLine + 1;
    constexpr std::size_t kHelpPaneFooterLine = kHelpPaneLines.size() - 1;
    constexpr std::size_t kHelpPaneSeparatorCount = 2;

    ftxui::Element popoverClearHalo(ftxui::Element popoverPtr)
    {
      return std::move(popoverPtr) | ftxui::borderEmpty | ftxui::clear_under;
    }
  } // namespace

  ftxui::Element renderKittyCoverArtPlaceholder(bool const hasCover)
  {
    using namespace ftxui;

    return vbox({
             text("Cover Art") | bold,
             separator(),
             hasCover ? filler() : text("No cover art") | dim | center,
           }) |
           border | size(WIDTH, EQUAL, kCoverArtPanelColumns) | size(HEIGHT, EQUAL, kCoverArtPanelRows);
  }

  void paintKittyCoverArt(ftxui::Box const& coverBox, std::vector<std::byte> const& png)
  {
    constexpr int kImageColumns = 24;
    constexpr int kImageRows = 12;

    if (coverBox.x_max <= coverBox.x_min || coverBox.y_max <= coverBox.y_min)
    {
      return;
    }

    auto const panelWidth = coverBox.x_max - coverBox.x_min + 1;
    auto const column = coverBox.x_min + std::max(1, (panelWidth - kImageColumns) / 2);
    auto const row = coverBox.y_min + 3;

    std::print("\033[s\033[{};{}H{}\033[u", row + 1, column + 1, kittyImageEscape(png, kImageColumns, kImageRows));
    std::fflush(stdout);
  }

  ftxui::Element centerPopover(ftxui::Element popoverPtr)
  {
    using namespace ftxui;

    return vbox({
      filler(),
      hbox({
        filler(),
        popoverClearHalo(std::move(popoverPtr)),
        filler(),
      }),
      filler(),
    });
  }

  std::int32_t detailPaneColumns(TrackListItem const* const selectedTrack, std::int32_t const terminalColumns)
  {
    auto contentColumns = std::max(kCoverArtPanelColumns, cellWidth("Track Detail"));

    if (selectedTrack == nullptr)
    {
      contentColumns = std::max(contentColumns, cellWidth("No track selected"));
    }
    else
    {
      for (auto const& line : trackDetailLines(selectedTrack->row))
      {
        contentColumns = std::max(contentColumns, kDetailPaneLabelColumns + cellWidth(line.value));
      }
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element detailPane(TrackListItem const* selectedTrack, ftxui::Element coverElementPtr, std::int32_t columns)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = detailPaneColumns(selectedTrack, 0);
    }

    auto detailElements = Elements{};

    if (selectedTrack == nullptr)
    {
      detailElements.push_back(text("No track selected") | dim);
    }
    else
    {
      for (auto const& line : trackDetailLines(selectedTrack->row))
      {
        detailElements.push_back(hbox({
          text(line.label + ": ") | dim | size(WIDTH, EQUAL, kDetailPaneLabelColumns),
          text(line.value) | flex,
        }));
      }
    }

    return style::popupPanel("Track Detail",
                             vbox({
                               std::move(coverElementPtr),
                               separator(),
                               vbox(std::move(detailElements)) | frame | flex,
                             })) |
           size(WIDTH, EQUAL, columns);
  }

  std::int32_t helpPaneColumns(std::int32_t const terminalColumns)
  {
    std::int32_t contentColumns = 0;

    for (auto const line : kHelpPaneLines)
    {
      contentColumns = std::max(contentColumns, cellWidth(line));
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element helpPane(std::int32_t columns)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = helpPaneColumns(0);
    }

    auto rows = Elements{};
    rows.reserve(kHelpPaneLines.size() + kHelpPaneSeparatorCount);

    for (std::size_t line = kHelpPaneCommandFirstLine; line < kHelpPaneFooterLine; ++line)
    {
      rows.push_back(text(std::string{kHelpPaneLines[line]}));
    }

    rows.push_back(separator());
    rows.push_back(text(std::string{kHelpPaneLines[kHelpPaneFooterLine]}) | dim);

    return style::popupPanel(kHelpPaneLines[kHelpPaneTitleLine], vbox(std::move(rows))) | size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
