// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "SelectableList.h"

#include "Style.h"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    ftxui::Element applySelectableListChrome(ftxui::Element listPtr, SelectableListOptions const& options)
    {
      using namespace ftxui;

      listPtr = std::move(listPtr) | focusPosition(0, std::max(0, options.focusRow));

      if (options.scrollIndicator)
      {
        listPtr = std::move(listPtr) | vscroll_indicator;
      }

      if (options.framed)
      {
        listPtr = std::move(listPtr) | frame;
      }

      if (options.height > 0)
      {
        listPtr = std::move(listPtr) | size(HEIGHT, EQUAL, options.height);
      }

      if (options.flex)
      {
        listPtr = std::move(listPtr) | ftxui::flex;
      }

      return listPtr;
    }
  } // namespace

  ftxui::Element selectableList(std::vector<SelectableListRow> rows, SelectableListOptions options)
  {
    using namespace ftxui;

    if (rows.empty())
    {
      auto emptyPtr = text(std::string{options.emptyText}) | dim;
      auto listPtr = options.centerEmpty ? vbox({std::move(emptyPtr)}) | center : vbox({std::move(emptyPtr)});
      return applySelectableListChrome(std::move(listPtr), options);
    }

    auto elements = Elements{};
    elements.reserve(rows.size());

    for (auto& row : rows)
    {
      auto rowPtr = std::move(row.elementPtr);

      if (row.selected)
      {
        rowPtr = std::move(rowPtr) | style::selected();
      }

      if (row.box != nullptr)
      {
        rowPtr = std::move(rowPtr) | reflect(*row.box);
      }

      elements.push_back(std::move(rowPtr));
    }

    return applySelectableListChrome(vbox(std::move(elements)), options);
  }
} // namespace ao::tui
