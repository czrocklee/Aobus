// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TextField.h"

#include "MouseBindings.h"
#include "TextFieldModel.h"
#include <ao/utility/UnicodeText.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <string>

namespace ao::tui
{
  ftxui::Element textFieldValue(TextFieldModel const& field, ftxui::Box* const origin, bool const focused)
  {
    using namespace ftxui;
    auto const& value = field.value();

    if (!focused)
    {
      return text(value) | (origin == nullptr ? nothing : reflectLayout(*origin));
    }

    auto const cursor = field.cursor();
    auto const nextRes = utility::nextUtf8GraphemeBoundary(value, cursor);
    auto const end = nextRes ? *nextRes : cursor;
    return hbox({text(value.substr(0, cursor)),
                 text(end > cursor ? value.substr(cursor, end - cursor) : std::string{" "}) | inverted | focus,
                 text(value.substr(end))}) |
           (origin == nullptr ? nothing : reflectLayout(*origin)) | xframe;
  }
} // namespace ao::tui
