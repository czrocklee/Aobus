// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Model.h"
#include "ShellModel.h"

#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::rt
{
  struct CompletionResult;
} // namespace ao::rt

namespace ao::tui
{
  inline constexpr std::int32_t kPresentationPanelColumns = 48;
  inline constexpr std::int32_t kPresentationPanelListRows = 10;
  inline constexpr std::int32_t kPresentationPanelChromeRows = 6;
  inline constexpr std::int32_t kPresentationPanelRows = kPresentationPanelListRows + kPresentationPanelChromeRows;
  inline constexpr std::int32_t kDefaultStatusBarColumns = 140;

  struct PresentationRowBox final
  {
    std::int32_t rowIndex = -1;
    ftxui::Box box{};
  };

  struct StatusBarViewState final
  {
    std::string statusMessage{};
    std::size_t trackCount = 0;
    std::int32_t selectedTrack = 0;
    std::int32_t terminalColumns = kDefaultStatusBarColumns;
    std::string filterDraft{};
    ShellModel const* shell = nullptr;
    ftxui::Box* commandBox = nullptr;
  };

  ftxui::Element renderKittyCoverArtPlaceholder(bool hasCover);
  void paintKittyCoverArt(ftxui::Box const& coverBox, std::vector<std::byte> const& png);

  ftxui::Element bottomPopover(ftxui::Element popoverPtr);
  ftxui::Element topPopover(ftxui::Element popoverPtr);
  std::int32_t detailPaneColumns(TrackListItem const* selectedTrack, std::int32_t terminalColumns);
  ftxui::Element detailPane(TrackListItem const* selectedTrack,
                            ftxui::Element coverElementPtr,
                            std::int32_t columns = 0);
  std::int32_t helpPaneColumns(std::int32_t terminalColumns);
  ftxui::Element helpPane(std::int32_t columns = 0);
  std::int32_t commandCompletionPanelColumns(rt::CompletionResult const& completion, std::int32_t terminalColumns);
  ftxui::Element commandCompletionPanel(rt::CompletionResult const& completion,
                                        std::int32_t selectedIndex,
                                        std::int32_t columns = 0);
  std::int32_t presentationPanelColumns(std::vector<PresentationNavItem> const& items,
                                        std::string_view activePresentationId,
                                        std::int32_t terminalColumns);
  ftxui::Element presentationPanel(std::vector<PresentationNavItem> const& items,
                                   std::string_view activePresentationId,
                                   std::int32_t selectedIndex,
                                   std::vector<PresentationRowBox>* rowBoxes = nullptr,
                                   std::int32_t columns = 0);
  ftxui::Element statusBar(StatusBarViewState const& state);
} // namespace ao::tui
