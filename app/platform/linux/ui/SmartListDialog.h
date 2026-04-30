// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "platform/linux/ui/QueryExpressionBox.h"
#include <rs/model/ListDraft.h>

#include <gtkmm.h>

#include <memory>
#include <optional>
#include <string>

namespace rs::library
{
  class MusicLibrary;
  class ListView;
}

namespace rs::model
{
  class AllTrackIdsList;
  class FilteredTrackIdList;
  class SmartListEngine;
  class TrackIdList;
}

namespace app::ui
{
  class TrackListAdapter;
  class TrackRowDataProvider;

  class SmartListDialog final : public Gtk::Dialog
  {
  public:
    SmartListDialog(Gtk::Window& parent,
                    rs::library::MusicLibrary& musicLibrary,
                    rs::model::AllTrackIdsList& allTrackIds,
                    rs::model::TrackIdList& parentMembershipList,
                    rs::ListId parentListId,
                    TrackRowDataProvider const& provider);
    virtual ~SmartListDialog() override;

    // Populate dialog fields from an existing list for editing
    void populate(rs::ListId id, rs::library::ListView const& view);

    // Returns the ListId for update (0 if creating a new list)
    rs::ListId editListId() const;

    // Returns a ListDraft populated from the dialog fields
    rs::model::ListDraft draft() const;

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
    rs::library::MusicLibrary& _musicLibrary;
    rs::model::AllTrackIdsList& _allTrackIds;
    rs::model::TrackIdList& _parentMembershipList;
    rs::ListId _parentListId;
    TrackRowDataProvider const& _rowDataProvider;
    std::unique_ptr<rs::model::FilteredTrackIdList> _previewFilteredList;
    std::unique_ptr<rs::model::SmartListEngine> _previewEngine;
    std::unique_ptr<TrackListAdapter> _previewAdapter;
    bool _expressionValid = true;

    // Edit mode state
    std::optional<rs::ListId> _editListId;
  };

} // namespace app::ui
