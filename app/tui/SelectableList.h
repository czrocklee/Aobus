// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  struct SelectableListRow final
  {
    ftxui::Element elementPtr{};
    bool selected = false;
    ftxui::Box* box = nullptr;
  };

  struct SelectableListOptions final
  {
    std::int32_t focusRow = 0;
    std::int32_t height = 0;
    std::string_view emptyText{};
    bool framed = true;
    /**
     * @brief Whether the window may scroll horizontally as well as vertically.
     *
     * A frame lays its child out on an unbounded canvas, so a list whose rows
     * own their own horizontal viewport turns this off and gets laid out
     * against the visible width instead.
     */
    bool horizontalScroll = true;
    bool scrollIndicator = true;
    bool flex = false;
    bool centerEmpty = false;
    ftxui::Box* viewportBox = nullptr;
  };

  ftxui::Element selectableList(std::vector<SelectableListRow> rows, SelectableListOptions options = {});
} // namespace ao::tui
