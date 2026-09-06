// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ao::tui
{
  /**
   * @brief One single-line editable value with a grapheme-cluster cursor.
   *
   * FTXUI supplies key events, not an editable buffer, so the metadata editor
   * owns its own text state. Every position this model exposes is a UTF-8 byte
   * offset that sits on an extended grapheme boundary, which is what keeps a
   * combining mark, a flag, or a joined emoji from being cut in half by a
   * cursor move or a Backspace.
   *
   * Mutations report whether the value actually changed, because the editor
   * turns a field's Apply intent on for an accepted edit and must not turn it
   * on for a rejected paste or a Backspace at the start of the value.
   */
  class TuiTextFieldModel final
  {
  public:
    TuiTextFieldModel() = default;
    explicit TuiTextFieldModel(std::string value);

    std::string const& value() const noexcept { return _value; }
    /// The cursor as a UTF-8 byte offset into @ref value.
    std::size_t cursor() const noexcept { return _cursor; }
    bool empty() const noexcept { return _value.empty(); }

    /// Replaces the value and parks the cursor at its end. Invalid UTF-8 loads as an empty value.
    void reset(std::string value);

    /**
     * @brief Inserts @p text at the cursor, reporting whether it was accepted.
     *
     * Rejects invalid UTF-8, line breaks (U+2028 and U+2029 included), and
     * terminal control characters whole rather than sanitizing them, so a
     * refused string leaves the value and cursor untouched.
     *
     * The atomicity is per call, and FTXUI implements no bracketed paste, so a
     * pasted string does not arrive here as one insert: the terminal replays it
     * as ordinary key events and its newlines arrive as Return. A multi-line
     * paste is therefore not refused as a unit, and this model cannot tell one
     * from typing.
     */
    bool insert(std::string_view text);

    /**
     * @brief Replaces the slice [begin, end) with @p text, reporting whether accepted.
     *
     * Validates that begin <= end <= value.size(), that begin and end lie on extended
     * grapheme cluster boundaries, and that text contains valid single-line UTF-8
     * without control characters. If invalid, refuses atomically, preserving value and cursor.
     * On acceptance, parks the cursor at the settled boundary after the inserted text.
     */
    bool replaceRange(std::size_t begin, std::size_t end, std::string_view text);

    /// Removes the cluster before the cursor; false at the start of the value.
    bool backspace();
    /// Removes the cluster at the cursor; false at the end of the value.
    bool deleteForward();

    bool moveLeft();
    bool moveRight();
    bool moveToBegin();
    bool moveToEnd();

  private:
    /// The nearest grapheme boundary at or after @p offset in the current value.
    std::size_t settledCursor(std::size_t offset) const;

    std::string _value{};
    std::size_t _cursor = 0;
  };
} // namespace ao::tui
