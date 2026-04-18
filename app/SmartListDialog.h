// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "QueryExpressionBox.h"
#include "model/ListDraft.h"

#include <gtkmm.h>

#include <memory>
#include <optional>
#include <string>

namespace rs::core
{
  class MusicLibrary;
  class ListView;
}

namespace app::model
{
  class AllTrackIdsList;
  class FilteredTrackIdList;
  class SmartListEngine;
  class TrackIdList;
  class TrackRowDataProvider;
}

class TrackListAdapter;

class SmartListDialog final : public Gtk::Dialog
{
	public:
  SmartListDialog(Gtk::Window& parent,
                  rs::core::MusicLibrary& musicLibrary,
                  app::model::AllTrackIdsList& allTrackIds,
                  app::model::TrackIdList& parentMembershipList,
                  rs::core::ListId parentListId);
  virtual ~SmartListDialog() override;

  // Populate dialog fields from an existing list for editing
  void populate(rs::core::ListId id, rs::core::ListView const& view);

  // Returns the ListId for update (0 if creating a new list)
  rs::core::ListId editListId() const;

  // Returns a ListDraft populated from the dialog fields
  app::model::ListDraft draft() const;

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
  rs::core::MusicLibrary* _musicLibrary;
  app::model::AllTrackIdsList* _allTrackIds;
  app::model::TrackIdList* _parentMembershipList;
  rs::core::ListId _parentListId;
  std::shared_ptr<app::model::TrackRowDataProvider> _rowDataProvider;
  std::unique_ptr<app::model::SmartListEngine> _previewEngine;
  std::unique_ptr<app::model::FilteredTrackIdList> _previewFilteredList;
  std::shared_ptr<TrackListAdapter> _previewAdapter;
  bool _expressionValid = true;

  // Edit mode state
  std::optional<rs::core::ListId> _editListId;
};
