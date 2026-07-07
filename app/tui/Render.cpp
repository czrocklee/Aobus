// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Render.h"

#include "CoverArt.h"
#include "Model.h"
#include "ShellModel.h"
#include "TextCell.h"
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionText.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <optional>
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
    constexpr std::int32_t kPresentationPanelMarkerColumns = 2;
    constexpr std::int32_t kPresentationPanelScrollIndicatorColumns = 1;

    std::string presentationPanelRowText(PresentationNavItem const& item)
    {
      auto label = item.label;

      if (!item.detail.empty())
      {
        label.append(" - ");
        label.append(item.detail);
      }

      return label;
    }

    constexpr auto kHelpPaneLines = std::to_array<std::string_view>({
      "Commands",
      "/text              quick filter",
      "/lists or /l       choose list",
      "/detail or /d      show selected track detail",
      "/pipeline or /a    show audio pipeline",
      "/output or /o      choose output device",
      "/views or /v       choose presentation",
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

  ftxui::Element bottomPopover(ftxui::Element popoverPtr)
  {
    using namespace ftxui;

    return vbox({
      filler(),
      hbox({
        filler(),
        std::move(popoverPtr) | clear_under,
      }),
    });
  }

  ftxui::Element topPopover(ftxui::Element popoverPtr)
  {
    using namespace ftxui;

    return vbox({
      hbox({
        filler(),
        std::move(popoverPtr) | clear_under,
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

    return panelColumnsForContent(contentColumns, terminalColumns);
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

    return vbox({
             std::move(coverElementPtr),
             separator(),
             text("Track Detail") | bold,
             separator(),
             vbox(std::move(detailElements)) | frame | flex,
           }) |
           border | size(WIDTH, EQUAL, columns);
  }

  std::int32_t helpPaneColumns(std::int32_t const terminalColumns)
  {
    std::int32_t contentColumns = 0;

    for (auto const line : kHelpPaneLines)
    {
      contentColumns = std::max(contentColumns, cellWidth(line));
    }

    return panelColumnsForContent(contentColumns, terminalColumns);
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
    rows.push_back(text(std::string{kHelpPaneLines[kHelpPaneTitleLine]}) | bold);
    rows.push_back(separator());

    for (std::size_t line = kHelpPaneCommandFirstLine; line < kHelpPaneFooterLine; ++line)
    {
      rows.push_back(text(std::string{kHelpPaneLines[line]}));
    }

    rows.push_back(separator());
    rows.push_back(text(std::string{kHelpPaneLines[kHelpPaneFooterLine]}) | dim);

    return vbox(std::move(rows)) | border | size(WIDTH, EQUAL, columns);
  }

  std::int32_t commandCompletionPanelColumns(rt::CompletionResult const& completion, std::int32_t const terminalColumns)
  {
    std::int32_t contentColumns = 0;

    for (auto const& item : completion.items)
    {
      contentColumns = std::max(contentColumns, cellWidth(item.displayText) + cellWidth(item.detail));
    }

    return panelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element commandCompletionPanel(rt::CompletionResult const& completion,
                                        std::int32_t const selectedIndex,
                                        std::int32_t columns)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = commandCompletionPanelColumns(completion, 0);
    }

    std::int32_t detailColumns = 0;

    for (auto const& item : completion.items)
    {
      detailColumns = std::max(detailColumns, cellWidth(item.detail));
    }

    detailColumns = std::min(detailColumns, std::max(0, columns - kPanelBorderColumns));

    auto rows = Elements{};
    rows.reserve(completion.items.size());

    for (std::size_t index = 0; index < completion.items.size(); ++index)
    {
      auto const& item = completion.items[index];
      auto cells = Elements{};
      cells.reserve(2);
      cells.push_back(text(item.displayText) | flex);

      if (detailColumns > 0)
      {
        cells.push_back(text(item.detail) | dim | size(WIDTH, EQUAL, detailColumns));
      }

      auto rowPtr = hbox(std::move(cells));

      if (std::cmp_equal(index, selectedIndex))
      {
        rowPtr = rowPtr | inverted;
      }

      rows.push_back(std::move(rowPtr));
    }

    return vbox(std::move(rows)) | border | size(WIDTH, EQUAL, columns);
  }

  std::int32_t presentationPanelColumns(std::vector<PresentationNavItem> const& items,
                                        std::string_view const activePresentationId,
                                        std::int32_t const terminalColumns)
  {
    auto contentColumns = std::max(cellWidth("No views available") + kPresentationPanelScrollIndicatorColumns,
                                   cellWidth(overlayHint(Overlay::PresentationPanel)));
    contentColumns =
      std::max(contentColumns, cellWidth("Views") + cellWidth(presentationDisplayId(activePresentationId)));

    for (auto const& item : items)
    {
      contentColumns = std::max(contentColumns,
                                kPresentationPanelMarkerColumns + cellWidth(presentationPanelRowText(item)) +
                                  kPresentationPanelScrollIndicatorColumns);
    }

    return panelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element presentationPanel(std::vector<PresentationNavItem> const& items,
                                   std::string_view const activePresentationId,
                                   std::int32_t const selectedIndex,
                                   std::vector<PresentationRowBox>* const rowBoxes,
                                   std::int32_t const columns)
  {
    using namespace ftxui;

    auto const panelColumns = columns <= 0 ? presentationPanelColumns(items, activePresentationId, 0) : columns;

    auto rows = Elements{};
    auto listRows = Elements{};
    std::int32_t focusRow = 0;

    if (rowBoxes != nullptr)
    {
      rowBoxes->clear();
      rowBoxes->reserve(items.size());
    }

    rows.push_back(hbox({
      text("Views") | bold | flex,
      text(presentationDisplayId(activePresentationId)) | bold,
    }));
    rows.push_back(separator());

    if (items.empty())
    {
      listRows.push_back(text("No views available") | dim);
    }

    for (std::size_t index = 0; index < items.size(); ++index)
    {
      auto const& item = items[index];
      auto label = presentationPanelRowText(item);

      auto rowPtr = hbox({
        text(item.id == activePresentationId ? "* " : "  "),
        text(std::move(label)) | flex,
      });

      if (std::cmp_equal(index, selectedIndex))
      {
        focusRow = static_cast<std::int32_t>(listRows.size());
        rowPtr = rowPtr | inverted | bold;
      }

      if (rowBoxes != nullptr)
      {
        rowBoxes->push_back(PresentationRowBox{.rowIndex = static_cast<std::int32_t>(index)});
        rowPtr = std::move(rowPtr) | reflect(rowBoxes->back().box);
      }

      listRows.push_back(std::move(rowPtr));
    }

    rows.push_back(vbox(std::move(listRows)) | focusPosition(0, focusRow) | vscroll_indicator | frame |
                   size(HEIGHT, EQUAL, kPresentationPanelListRows));
    rows.push_back(separator());
    rows.push_back(text(std::string{overlayHint(Overlay::PresentationPanel)}) | dim);

    return vbox(std::move(rows)) | border | size(WIDTH, EQUAL, panelColumns);
  }

  ftxui::Element statusBar(StatusBarViewState const& state)
  {
    using namespace ftxui;

    constexpr std::int32_t kSingleLineStatusColumns = 120;

    auto const& shell = *state.shell;
    auto const selection = selectionSummary(state.trackCount, state.selectedTrack);

    if (shell.commandActive())
    {
      auto suffix = std::string{};

      if (auto const& optCompletion = shell.commandCompletion(); optCompletion && !optCompletion->items.empty())
      {
        auto const selected = std::clamp<std::int32_t>(
          shell.commandCompletionSelection(), 0, static_cast<std::int32_t>(optCompletion->items.size()) - 1);
        auto const& item = optCompletion->items[static_cast<std::size_t>(selected)];
        auto const replaceBegin = std::min(optCompletion->replaceBegin, shell.commandDraft().size());
        auto const replaceEnd = std::min(optCompletion->replaceEnd, shell.commandDraft().size());
        auto const current = std::string_view{shell.commandDraft()}.substr(replaceBegin, replaceEnd - replaceBegin);

        if (!current.empty() && replaceEnd == shell.commandDraft().size() &&
            rt::completionStartsWithInsensitive(item.insertText, current))
        {
          suffix = item.insertText.substr(current.size());
        }
      }

      auto commandLinePtr = hbox({
        text("/" + shell.commandDraft()) | bold,
        text(suffix) | dim,
        text("_") | bold,
      });

      if (state.commandBox != nullptr)
      {
        commandLinePtr = std::move(commandLinePtr) | reflect(*state.commandBox);
      }

      return hbox({
        std::move(commandLinePtr) | flex,
        text("Tab complete  Enter run  Esc cancel") | dim,
      });
    }

    auto const overlay = shell.overlay();
    auto const interactionHint = std::string{overlayHint(overlay)};
    auto const contextLabel = overlay == Overlay::None ? std::string{} : overlayLabel(overlay);

    auto hint = std::string{};

    if (!state.filterDraft.empty())
    {
      hint = std::format("Filter: {}  ", state.filterDraft);
    }

    hint += interactionHint;

    if (overlay != Overlay::None)
    {
      return hbox({
        text(state.statusMessage) | flex,
        text(contextLabel) | bold,
        text("  "),
        text(hint) | dim,
        text("  "),
        text(selection),
      });
    }

    if (state.terminalColumns >= kSingleLineStatusColumns)
    {
      return hbox({
        text(state.statusMessage) | flex,
        text(hint) | dim,
        text("  "),
        text(selection),
      });
    }

    return vbox({
      hbox({
        text(state.statusMessage) | flex,
        text(selection),
      }),
      hbox({
        text(hint) | dim | flex,
      }),
    });
  }
} // namespace ao::tui
