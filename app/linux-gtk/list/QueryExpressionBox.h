// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "completion/EntryCompletionController.h"
#include <ao/rt/CompletionResult.h>
#include <ao/rt/QueryExpressionCompleter.h>

#include <gtkmm/box.h>
#include <gtkmm/entry.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace ao::rt
{
  class CompletionService;
}

namespace ao::gtk
{
  class QueryExpressionBox final : public Gtk::Box
  {
  public:
    explicit QueryExpressionBox(rt::CompletionService& completion);
    ~QueryExpressionBox() override;

    QueryExpressionBox(QueryExpressionBox const&) = delete;
    QueryExpressionBox& operator=(QueryExpressionBox const&) = delete;
    QueryExpressionBox(QueryExpressionBox&&) = delete;
    QueryExpressionBox& operator=(QueryExpressionBox&&) = delete;

    Gtk::Entry& entry() { return _entry; }
    Gtk::Entry const& entry() const { return _entry; }

  private:
    std::optional<rt::CompletionResult> complete(std::string_view text, std::size_t cursor);

    Gtk::Entry _entry;
    rt::QueryExpressionCompleter _completer;
    EntryCompletionController _completionController;
  };
} // namespace ao::gtk
