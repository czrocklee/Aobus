// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "QueryExpressionBox.h"
#include "model/ListDraft.h"

#include <gtkmm.h>

#include <memory>
#include <string>

namespace rs::core
{
  class MusicLibrary;
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

class NewListDialog final : public Gtk::Dialog
{
public:
  struct SourceChoice final
  {
    rs::core::ListId listId = rs::core::ListId{0};
    std::string name;
    std::string inheritedExpression;
    app::model::TrackIdList* source = nullptr;
  };

  NewListDialog(Gtk::Window& parent,
                rs::core::MusicLibrary& musicLibrary,
                app::model::AllTrackIdsList& allTrackIds,
                std::shared_ptr<app::model::TrackRowDataProvider> rowDataProvider,
                std::vector<SourceChoice> sourceChoices,
                rs::core::ListId defaultSourceListId = rs::core::ListId{0});
  virtual ~NewListDialog() override;

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
  SourceChoice const* selectedSourceChoice() const;

  Gtk::Entry _nameEntry;
  Gtk::Entry _descEntry;
  Gtk::DropDown _sourceDropDown;
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
  Glib::RefPtr<Gtk::StringList> _sourceItems;
  sigc::connection _exprTimeoutConnection;

  // Preview infrastructure
  rs::core::MusicLibrary* _musicLibrary;
  app::model::AllTrackIdsList* _allTrackIds;
  std::shared_ptr<app::model::TrackRowDataProvider> _rowDataProvider;
  std::vector<SourceChoice> _sourceChoices;
  std::unique_ptr<app::model::SmartListEngine> _previewEngine;
  std::unique_ptr<app::model::FilteredTrackIdList> _previewFilteredList;
  std::shared_ptr<TrackListAdapter> _previewAdapter;
  bool _sourceValid = false;
  bool _expressionValid = true;
};
