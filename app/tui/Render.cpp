// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Render.h"

#include "CoverArt.h"
#include "Model.h"
#include "ShellModel.h"
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionText.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  ftxui::Element renderKittyCoverArtPlaceholder(bool const hasCover)
  {
    constexpr int kPanelColumns = 30;
    constexpr int kPanelRows = 16;
    using namespace ftxui;

    return vbox({
             text("Cover Art") | bold,
             separator(),
             hasCover ? filler() : text("No cover art") | dim | center,
           }) |
           border | size(WIDTH, EQUAL, kPanelColumns) | size(HEIGHT, EQUAL, kPanelRows);
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

    std::cout << "\033[s" << std::format("\033[{};{}H", row + 1, column + 1)
              << kittyImageEscape(png, kImageColumns, kImageRows) << "\033[u" << std::flush;
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

  ftxui::Element anchoredPopover(ftxui::Box const anchor,
                                 std::int32_t const popoverColumns,
                                 std::int32_t const terminalColumns,
                                 ftxui::Element popoverPtr)
  {
    using namespace ftxui;

    auto const maxLeft = terminalColumns > popoverColumns ? terminalColumns - popoverColumns : 0;
    auto const left = std::clamp(anchor.x_min, 0, maxLeft);
    auto const top = std::max(0, anchor.y_max + 1);

    return vbox({
      filler() | size(HEIGHT, EQUAL, top),
      hbox({
        filler() | size(WIDTH, EQUAL, left),
        std::move(popoverPtr) | clear_under,
        filler(),
      }),
      filler(),
    });
  }

  ftxui::Element anchoredPopoverAbove(ftxui::Box const anchor,
                                      std::int32_t const popoverColumns,
                                      std::int32_t const terminalColumns,
                                      std::int32_t const terminalRows,
                                      std::int32_t const popoverRows,
                                      ftxui::Element popoverPtr)
  {
    using namespace ftxui;

    auto const maxLeft = terminalColumns > popoverColumns ? terminalColumns - popoverColumns : 0;
    auto const left = std::clamp(anchor.x_min, 0, maxLeft);
    auto const top = std::max(0, anchor.y_min - popoverRows);

    return vbox({
      filler() | size(HEIGHT, EQUAL, top),
      hbox({
        filler() | size(WIDTH, EQUAL, left),
        std::move(popoverPtr) | clear_under,
        filler(),
      }),
      filler() | size(HEIGHT, EQUAL, std::max(0, terminalRows - top - popoverRows)),
    });
  }

  ftxui::Element detailPane(TrackListItem const* selectedTrack, ftxui::Element coverElementPtr)
  {
    constexpr int kLabelColumns = 14;
    constexpr int kDetailPaneColumns = 34;
    using namespace ftxui;

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
          text(line.label + ": ") | dim | size(WIDTH, EQUAL, kLabelColumns),
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
           border | size(WIDTH, EQUAL, kDetailPaneColumns);
  }

  ftxui::Element helpPane()
  {
    constexpr int kHelpPaneColumns = 38;
    using namespace ftxui;

    return vbox({
             text("Commands") | bold,
             separator(),
             text("/text              quick filter"),
             text("/lists or /l       choose list"),
             text("/detail or /d      show selected track detail"),
             text("/quality or /a     show audio quality"),
             text("/output or /o      choose output device"),
             text("/current           reveal current track"),
             text("/view <id>         switch presentation"),
             text("/clear             clear filter"),
             text("/reload            reload active list"),
             text("/play /pause /stop playback"),
             text("/quit              exit"),
             separator(),
             text("Esc close  Enter run") | dim,
           }) |
           border | size(WIDTH, EQUAL, kHelpPaneColumns);
  }

  ftxui::Element commandCompletionPanel(rt::CompletionResult const& completion, std::int32_t const selectedIndex)
  {
    constexpr std::int32_t kDetailColumns = 18;
    using namespace ftxui;

    auto rows = Elements{};
    rows.reserve(completion.items.size());

    for (std::size_t index = 0; index < completion.items.size(); ++index)
    {
      auto const& item = completion.items[index];
      auto rowPtr = hbox({
        text(item.displayText) | flex,
        text(item.detail) | dim | size(WIDTH, EQUAL, kDetailColumns),
      });

      if (std::cmp_equal(index, selectedIndex))
      {
        rowPtr = rowPtr | inverted;
      }

      rows.push_back(std::move(rowPtr));
    }

    return vbox(std::move(rows)) | border;
  }

  ftxui::Element statusBar(StatusBarViewState const& state)
  {
    constexpr std::int32_t kCompactColumns = 110;
    using namespace std::literals;
    constexpr auto kShortcutText =
      "/ command  l lists  d detail  a quality  o output  Ctrl-L current  /view id  q quit"sv;
    using namespace ftxui;

    auto const& shell = *state.shell;
    auto const filter =
      state.filterDraft.empty() ? std::string{"filter:-"} : std::format("filter:{}", state.filterDraft);
    auto const presentation =
      state.presentationId.empty() ? std::string{"view:default"} : std::format("view:{}", state.presentationId);
    auto const selection = selectionSummary(state.trackCount, state.selectedTrack);
    auto const overlay = overlayLabel(shell.overlay());

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

    if (state.terminalColumns < kCompactColumns)
    {
      return vbox({
        hbox({
          text(state.statusMessage) | flex,
          text("Mode: " + overlay + "  ") | dim,
          text(selection),
        }),
        hbox({
          text(filter + "  " + presentation) | dim | flex,
          text(std::string{kShortcutText}) | dim,
        }),
      });
    }

    return hbox({
      text(state.statusMessage) | flex,
      text("Mode: " + overlay + "  ") | dim,
      text(filter + "  " + presentation + "  ") | dim,
      text(std::string{kShortcutText} + "  ") | dim,
      text(selection),
    });
  }
} // namespace ao::tui
