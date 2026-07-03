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
             text("/views or /v       choose presentation"),
             text("/current           reveal current track"),
             text("/view <id>         switch presentation"),
             text("{ / }              previous / next group"),
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

  ftxui::Element presentationPanel(std::vector<PresentationNavItem> const& items,
                                   std::string_view const activePresentationId,
                                   std::int32_t const selectedIndex,
                                   std::vector<PresentationRowBox>* const rowBoxes)
  {
    constexpr std::int32_t kIdColumns = 20;
    using namespace ftxui;

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
      auto rowPtr = hbox({
        text(item.id == activePresentationId ? "* " : "  "),
        text(item.label) | flex,
        text(item.id) | dim | size(WIDTH, EQUAL, kIdColumns),
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

    return vbox(std::move(rows)) | border | size(WIDTH, EQUAL, kPresentationPanelColumns);
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
