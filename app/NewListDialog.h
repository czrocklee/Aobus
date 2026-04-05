// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "model/ListDraft.h"

#include <gtkmm.h>

#include <functional>
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
class TrackRowDataProvider;
}

class TrackListAdapter;

class NewListDialog final : public Gtk::Dialog
{
public:
  NewListDialog(Gtk::Window& parent,
                rs::core::MusicLibrary& musicLibrary,
                app::model::AllTrackIdsList& allTrackIds,
                std::shared_ptr<app::model::TrackRowDataProvider> rowDataProvider);
  virtual ~NewListDialog() override;

  // Returns a ListDraft populated from the dialog fields
  app::model::ListDraft draft() const;

private:
  void setupUi();
  void setupPreview();
  void setupPreviewColumns();
  void updatePreview();

  Gtk::Entry _nameEntry;
  Gtk::Entry _descEntry;
  Gtk::Entry _exprEntry;
  Gtk::Button _okButton;
  Gtk::Button _cancelButton;
  Gtk::Box _leftPanel;
  Gtk::Box _rightPanel;
  Gtk::Label _matchCountLabel;
  Gtk::Label _errorLabel;
  Gtk::ScrolledWindow _previewScrolledWindow;
  Gtk::ColumnView _previewColumnView;
  sigc::connection _exprTimeoutConnection;

  // Preview infrastructure
  rs::core::MusicLibrary* _musicLibrary;
  app::model::AllTrackIdsList* _allTrackIds;
  std::shared_ptr<app::model::TrackRowDataProvider> _rowDataProvider;
  std::unique_ptr<app::model::FilteredTrackIdList> _previewFilteredList;
  std::shared_ptr<TrackListAdapter> _previewAdapter;
};