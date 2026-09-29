// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionItem.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::tui
{
  /// Candidate rows one anchored completion popup shows at a time.
  inline constexpr std::size_t kCompletionPopupPageSize = 6;
  /// Cells before the popup: the focus marker, the changed marker, and the label gutter.
  inline constexpr std::size_t kCompletionPopupIndentColumns = 5;

  /**
   * @brief The candidate selection and window state of one anchored completion popup.
   *
   * The terminal editors share one popup contract: arrows move the selection
   * by one, pages move selection and window together, and the window follows
   * whenever the selection would leave it.
   */
  class CompletionPopupSelection final
  {
  public:
    void reset() noexcept;
    /// Moves the selection to one clicked candidate row.
    void select(std::size_t candidateIndex) noexcept;
    /// Applies arrow and page navigation; reports false when @p event is neither.
    bool tryNavigate(ftxui::Event const& event, std::size_t itemCount);

    std::size_t selectedCandidate() const noexcept { return _selectedCandidate; }
    std::size_t windowStart() const noexcept { return _windowStart; }

  private:
    std::size_t _selectedCandidate = 0;
    std::size_t _windowStart = 0;
  };

  /**
   * @brief Draws the anchored candidate window below the completing row.
   *
   * @p candidateBoxes must already hold one box per item; each visible row
   * reflects its box so a click can accept that exact candidate.
   */
  ftxui::Element renderCompletionPopup(std::span<rt::CompletionItem const> items,
                                       CompletionPopupSelection const& selection,
                                       std::int32_t indentColumns,
                                       std::vector<ftxui::Box>& candidateBoxes);
} // namespace ao::tui
