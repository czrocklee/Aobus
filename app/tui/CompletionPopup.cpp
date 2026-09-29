// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CompletionPopup.h"

#include "SelectionNavigation.h"
#include "Style.h"
#include <ao/rt/completion/CompletionItem.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ao::tui
{
  void CompletionPopupSelection::reset() noexcept
  {
    _selectedCandidate = 0;
    _windowStart = 0;
  }

  void CompletionPopupSelection::select(std::size_t const candidateIndex) noexcept
  {
    _selectedCandidate = candidateIndex;
  }

  bool CompletionPopupSelection::tryNavigate(ftxui::Event const& event, std::size_t const itemCount)
  {
    std::int32_t delta = 0;
    auto const pageSize = static_cast<std::int32_t>(kCompletionPopupPageSize);

    if (event == ftxui::Event::ArrowUp)
    {
      delta = -1;
    }
    else if (event == ftxui::Event::ArrowDown)
    {
      delta = 1;
    }
    else if (event == ftxui::Event::PageUp)
    {
      delta = -pageSize;
    }
    else if (event == ftxui::Event::PageDown)
    {
      delta = pageSize;
    }
    else
    {
      return false;
    }

    _selectedCandidate =
      static_cast<std::size_t>(moveSelection(static_cast<std::int32_t>(_selectedCandidate), delta, itemCount));

    if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
    {
      auto const windowCount = itemCount - std::min(itemCount, kCompletionPopupPageSize) + 1;
      _windowStart =
        static_cast<std::size_t>(moveSelection(static_cast<std::int32_t>(_windowStart), delta, windowCount));
    }

    if (_selectedCandidate < _windowStart)
    {
      _windowStart = _selectedCandidate;
    }
    else if (_selectedCandidate >= _windowStart + kCompletionPopupPageSize)
    {
      _windowStart = _selectedCandidate - kCompletionPopupPageSize + 1;
    }

    return true;
  }

  ftxui::Element renderCompletionPopup(std::span<rt::CompletionItem const> const items,
                                       CompletionPopupSelection const& selection,
                                       std::int32_t const indentColumns,
                                       std::vector<ftxui::Box>& candidateBoxes)
  {
    using namespace ftxui;

    auto const windowStart = selection.windowStart();
    auto const windowEnd = std::min(items.size(), windowStart + kCompletionPopupPageSize);
    auto candidateElements = Elements{};

    for (std::size_t candIndex = windowStart; candIndex < windowEnd; ++candIndex)
    {
      auto const isCandidateSelected = candIndex == selection.selectedCandidate();
      auto itemRowPtr = hbox({text(isCandidateSelected ? "> " : "  "), text(items[candIndex].displayText)});

      if (isCandidateSelected)
      {
        itemRowPtr = std::move(itemRowPtr) | inverted;
      }

      candidateElements.push_back(std::move(itemRowPtr) | ftxui::reflect(candidateBoxes[candIndex]));
    }

    auto popupBoxPtr = style::panelBody(vbox(std::move(candidateElements))) | border | clear_under;

    return hbox({
      text(std::string(static_cast<std::size_t>(indentColumns), ' ')),
      std::move(popupBoxPtr),
    });
  }
} // namespace ao::tui
