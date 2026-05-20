// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "list/QueryExpressionBox.h"
#include "runtime/LibraryMutationService.h"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/columnview.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>

#include <memory>
#include <string>

namespace ao::library
{
  class MusicLibrary;
  class ListView;
}

namespace ao::rt
{
  class AppRuntime;
  class AllTracksSource;
  class SmartListSource;
  class SmartListEvaluator;
  class TrackSource;
}

namespace ao::gtk
{
  class TrackListAdapter;
  class TrackRowCache;

  class SmartListDialog final : public Gtk::Dialog
  {
  public:
    SmartListDialog(Gtk::Window& parent, rt::AppRuntime& runtime, ListId parentListId, TrackRowCache const& provider);
    ~SmartListDialog() override;

    SmartListDialog(SmartListDialog const&) = delete;
    SmartListDialog& operator=(SmartListDialog const&) = delete;
    SmartListDialog(SmartListDialog&&) = delete;
    SmartListDialog& operator=(SmartListDialog&&) = delete;

    // Populate dialog fields from an existing list for editing
    void populate(ListId id, library::ListView const& view);

    // Returns the ListId for update (0 if creating a new list)
    ListId editListId() const;

    // Returns a ListDraft populated from the dialog fields
    rt::LibraryMutationService::ListDraft draft() const;

    void setLocalExpression(std::string const& expression);

  private:
    void setupUi();
    void setupPreview();
    void setupPreviewColumns();
    void rebuildPreviewSource();
    void updateSourceLabels();
    void updateDialogState();
    void updatePreview();

    Gtk::Entry _nameEntry;
    Gtk::Entry _descEntry;
    QueryExpressionBox _exprBox;
    Gtk::Button _okButton;
    Gtk::Button _cancelButton;
    Gtk::Box _leftPanel;
    Gtk::Box _rightPanel;
    Gtk::Label _inheritedExprLabel;
    Gtk::Label _effectiveExprLabel;
    Gtk::Label _matchCountLabel;
    Gtk::Label _errorLabel;
    Gtk::ScrolledWindow _previewScrolledWindow;
    Gtk::ColumnView _previewColumnView;
    sigc::connection _exprTimeoutConnection;

    // Preview infrastructure
    rt::AppRuntime& _runtime;
    ListId _parentListId;
    TrackRowCache const& _trackRowCache;
    std::unique_ptr<rt::SmartListSource> _previewFilteredList;
    std::unique_ptr<rt::SmartListEvaluator> _previewEngine;
    std::unique_ptr<TrackListAdapter> _previewAdapter;
    bool _expressionValid = true;

    // Edit mode state
    ListId _editListId{kInvalidListId};
  };
} // namespace ao::gtk
