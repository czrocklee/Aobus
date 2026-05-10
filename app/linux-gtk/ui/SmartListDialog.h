// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "QueryExpressionBox.h"
#include <ao/model/ListDraft.h>

#include <gtkmm.h>

#include <memory>
#include <optional>
#include <string>

namespace ao::library
{
  class MusicLibrary;
  class ListView;
}

namespace ao::app
{
  class AppSession;
  class AllTracksSource;
  class SmartListSource;
  class SmartListEvaluator;
  class TrackSource;
}

namespace ao::gtk
{
  class TrackListAdapter;
  class TrackRowDataProvider;

  class SmartListDialog final : public Gtk::Dialog
  {
  public:
    SmartListDialog(Gtk::Window& parent,
                    ao::app::AppSession& session,
                    ao::ListId parentListId,
                    TrackRowDataProvider const& provider);
    ~SmartListDialog() override;

    // Populate dialog fields from an existing list for editing
    void populate(ao::ListId id, ao::library::ListView const& view);

    // Returns the ListId for update (0 if creating a new list)
    ao::ListId editListId() const;

    // Returns a ListDraft populated from the dialog fields
    ao::model::ListDraft draft() const;

    void setLocalExpression(std::string expression);

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
    ao::app::AppSession& _session;
    ao::ListId _parentListId;
    TrackRowDataProvider const& _rowDataProvider;
    std::unique_ptr<ao::app::SmartListSource> _previewFilteredList;
    std::unique_ptr<ao::app::SmartListEvaluator> _previewEngine;
    std::unique_ptr<TrackListAdapter> _previewAdapter;
    bool _expressionValid = true;

    // Edit mode state
    std::optional<ao::ListId> _optEditListId;
  };
} // namespace ao::gtk
