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
    bool scrollIndicator = true;
    bool flex = false;
    bool centerEmpty = false;
  };

  ftxui::Element selectableList(std::vector<SelectableListRow> rows, SelectableListOptions options = {});
} // namespace ao::tui
