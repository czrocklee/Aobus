// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Model.h"
#include "ShellModel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ftxui
{
  struct Box;
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::rt
{
  struct CompletionResult;
} // namespace ao::rt

namespace ao::tui
{
  struct StatusBarViewState final
  {
    std::string statusMessage{};
    std::size_t trackCount = 0;
    std::int32_t selectedTrack = 0;
    std::string filterDraft{};
    std::string presentationId{};
    ShellModel const* shell = nullptr;
    ftxui::Box* commandBox = nullptr;
    std::int32_t terminalColumns = 0;
  };

  ftxui::Element renderKittyCoverArtPlaceholder(bool hasCover);
  void paintKittyCoverArt(ftxui::Box const& coverBox, std::vector<std::byte> const& png);

  ftxui::Element bottomPopover(ftxui::Element popoverPtr);
  ftxui::Element topPopover(ftxui::Element popoverPtr);
  ftxui::Element anchoredPopover(ftxui::Box anchor,
                                 std::int32_t popoverColumns,
                                 std::int32_t terminalColumns,
                                 ftxui::Element popoverPtr);
  ftxui::Element anchoredPopoverAbove(ftxui::Box anchor,
                                      std::int32_t popoverColumns,
                                      std::int32_t terminalColumns,
                                      std::int32_t terminalRows,
                                      std::int32_t popoverRows,
                                      ftxui::Element popoverPtr);
  ftxui::Element detailPane(TrackListItem const* selectedTrack, ftxui::Element coverElementPtr);
  ftxui::Element helpPane();
  ftxui::Element commandCompletionPanel(rt::CompletionResult const& completion, std::int32_t selectedIndex);
  ftxui::Element statusBar(StatusBarViewState const& state);
} // namespace ao::tui
