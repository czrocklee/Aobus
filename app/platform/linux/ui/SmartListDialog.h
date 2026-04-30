// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/model/ListDraft.h"
#include "platform/linux/ui/QueryExpressionBox.h"

#include <gtkmm.h>

#include <memory>
#include <optional>
#include <string>

namespace rs::core
{
  class MusicLibrary;
  class ListView;
}

namespace app::core::model
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
                    rs::core::MusicLibrary& musicLibrary,
                    app::core::model::AllTrackIdsList& allTrackIds,
                    app::core::model::TrackIdList& parentMembershipList,
                    rs::core::ListId parentListId,
                    TrackRowDataProvider const& provider);
    virtual ~SmartListDialog() override;

    // Populate dialog fields from an existing list for editing
    void populate(rs::core::ListId id, rs::core::ListView const& view);

    // Returns the ListId for update (0 if creating a new list)
    rs::core::ListId editListId() const;

    // Returns a ListDraft populated from the dialog fields
    app::core::model::ListDraft draft() const;

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
    rs::core::MusicLibrary& _musicLibrary;
    app::core::model::AllTrackIdsList& _allTrackIds;
    app::core::model::TrackIdList& _parentMembershipList;
    rs::core::ListId _parentListId;
    TrackRowDataProvider const& _rowDataProvider;
    std::unique_ptr<app::core::model::FilteredTrackIdList> _previewFilteredList;
    std::unique_ptr<app::core::model::SmartListEngine> _previewEngine;
    std::unique_ptr<TrackListAdapter> _previewAdapter;
    bool _expressionValid = true;

    // Edit mode state
    std::optional<rs::core::ListId> _editListId;
  };

} // namespace app::ui
