// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

namespace ao::tui
{
  class TextFieldModel;

  /// Draws one grapheme caret and retains the unscrolled origin for mouse positioning.
  ftxui::Element textFieldValue(TextFieldModel const& field, ftxui::Box* origin = nullptr, bool focused = true);
} // namespace ao::tui
