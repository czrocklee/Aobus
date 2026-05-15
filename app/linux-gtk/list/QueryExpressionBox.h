// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <gtkmm.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::gtk
{
  class QueryExpressionBox final : public Gtk::Box
  {
  public:
    explicit QueryExpressionBox(library::MusicLibrary& musicLibrary);
    ~QueryExpressionBox() override;

    QueryExpressionBox(QueryExpressionBox const&) = delete;
    QueryExpressionBox& operator=(QueryExpressionBox const&) = delete;
    QueryExpressionBox(QueryExpressionBox&&) = delete;
    QueryExpressionBox& operator=(QueryExpressionBox&&) = delete;

    void refreshCompletionData();

    Gtk::Entry& entry() { return _entry; }
    Gtk::Entry const& entry() const { return _entry; }

  private:
    void setupCompletion();
    void updateCompletion();
    void hideCompletion();
    void applySelectedCompletion();
    bool moveCompletionSelection(int delta);

    Gtk::Entry _entry;
    Gtk::Popover _completionPopover;
    Gtk::ScrolledWindow _completionScrolledWindow;
    Gtk::ListView _completionListView;
    Glib::RefPtr<Gtk::StringList> _completionItems;
    Glib::RefPtr<Gtk::SingleSelection> _completionSelection;
    library::MusicLibrary& _musicLibrary;
    std::vector<std::string> _availableTags;
    std::vector<std::string> _availableCustomKeys;
    std::int32_t _completionTokenStart = -1;
    bool _suppressNextCompletionUpdate = false;

    // Layout constants
    static constexpr int kCompletionWidth = 260;
    static constexpr int kCompletionHeight = 180;
  };
} // namespace ao::gtk
