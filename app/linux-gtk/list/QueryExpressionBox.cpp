// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/QueryExpressionBox.h"

#include "completion/EntryCompletionController.h"
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace ao::gtk
{
  QueryExpressionBox::QueryExpressionBox(rt::CompletionService& completion)
    : Gtk::Box{Gtk::Orientation::VERTICAL}
    , _completer{completion}
    , _completionController{_entry,
                            [this](std::string_view text, std::size_t cursor) { return complete(text, cursor); }}
  {
    _entry.add_css_class("ao-query-expression-entry");
    _entry.set_hexpand(true);
    append(_entry);
  }

  QueryExpressionBox::~QueryExpressionBox() = default;

  std::optional<rt::CompletionResult> QueryExpressionBox::complete(std::string_view text, std::size_t cursor)
  {
    return _completer.complete(text, cursor);
  }
} // namespace ao::gtk
