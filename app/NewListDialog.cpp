// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "NewListDialog.h"

#include "TrackListAdapter.h"
#include "TrackRow.h"

#include "model/AllTrackIdsList.h"
#include "model/FilteredTrackIdList.h"
#include "model/TrackRowDataProvider.h"

#include <rs/core/MusicLibrary.h>

#include <algorithm>

NewListDialog::NewListDialog(Gtk::Window& parent,
                             rs::core::MusicLibrary& musicLibrary,
                             app::model::AllTrackIdsList& allTrackIds,
                             std::shared_ptr<app::model::TrackRowDataProvider> rowDataProvider)
  : _exprBox(musicLibrary)
  , _musicLibrary(&musicLibrary)
  , _allTrackIds(&allTrackIds)
  , _rowDataProvider(std::move(rowDataProvider))
{
  set_title("New List");
  set_transient_for(parent);
  set_modal(true);
  setupUi();
  setupPreview();
}

NewListDialog::~NewListDialog()
{
  _exprTimeoutConnection.disconnect();
}

void NewListDialog::setupUi()
{
  constexpr int kDialogWidth = 800;
  constexpr int kDialogHeight = 400;
  constexpr int kBoxSpacing = 8;
  constexpr int kBoxMargin = 12;
  constexpr int kButtonBoxSpacing = 6;

  set_default_size(kDialogWidth, kDialogHeight);

  // Main container: horizontal box (left: inputs, right: preview)
  auto mainBox = Gtk::Box(Gtk::Orientation::HORIZONTAL, kBoxSpacing * 2);
  mainBox.set_margin(kBoxMargin);

  // Left panel: input fields
  _leftPanel.set_orientation(Gtk::Orientation::VERTICAL);
  _leftPanel.set_spacing(kBoxSpacing);

  // Name field
  auto nameLabel = Gtk::Label("Name:");
  nameLabel.set_halign(Gtk::Align::START);
  _nameEntry.set_placeholder_text("List name");
  _leftPanel.append(nameLabel);
  _leftPanel.append(_nameEntry);

  // Description field
  auto descLabel = Gtk::Label("Description:");
  descLabel.set_halign(Gtk::Align::START);
  _descEntry.set_placeholder_text("Optional description");
  _leftPanel.append(descLabel);
  _leftPanel.append(_descEntry);

  // Expression field
  auto exprLabel = Gtk::Label("Expression:");
  exprLabel.set_halign(Gtk::Align::START);
  _exprBox.entry().set_placeholder_text("Query expression (type $, @, #, or %)");
  _exprBox.entry().signal_changed().connect([this]()
  {
    // Cancel any pending update
    _exprTimeoutConnection.disconnect();
    // Debounce: update preview after 100ms of inactivity
    _exprTimeoutConnection = Glib::signal_timeout().connect(
      [this]()
    {
      updatePreview();
      return false; // one-shot
    },
      100);
  });
  _leftPanel.append(exprLabel);
  _leftPanel.append(_exprBox);

  // Error label (shown below expression when invalid)
  _errorLabel.set_visible(false);
  _errorLabel.set_wrap(true);
  _errorLabel.set_halign(Gtk::Align::START);
  _leftPanel.append(_errorLabel);

  // Buttons
  auto buttonBox = Gtk::Box(Gtk::Orientation::HORIZONTAL, kButtonBoxSpacing);
  buttonBox.set_halign(Gtk::Align::END);
  buttonBox.set_margin_top(kBoxSpacing * 2);

  _cancelButton.set_label("Cancel");
  _cancelButton.signal_clicked().connect([this]() { response(Gtk::ResponseType::CANCEL); });

  _okButton.set_label("Create");
  _okButton.set_sensitive(false);
  _okButton.signal_clicked().connect([this]() { response(Gtk::ResponseType::OK); });

  // Enable OK button when name is filled
  _nameEntry.signal_changed().connect([this]() { _okButton.set_sensitive(!_nameEntry.get_text().empty()); });

  buttonBox.append(_cancelButton);
  buttonBox.append(_okButton);
  _leftPanel.append(buttonBox);

  // Right panel: preview area
  _rightPanel.set_orientation(Gtk::Orientation::VERTICAL);
  _rightPanel.set_spacing(kBoxSpacing);
  _rightPanel.set_hexpand(true);
  _rightPanel.set_vexpand(true);

  auto previewLabel = Gtk::Label("Preview:");
  previewLabel.set_halign(Gtk::Align::START);
  _rightPanel.append(previewLabel);

  _matchCountLabel.set_halign(Gtk::Align::START);
  _matchCountLabel.set_markup("<i>Enter an expression to see matches</i>");
  _rightPanel.append(_matchCountLabel);

  _previewScrolledWindow.set_vexpand(true);
  _previewScrolledWindow.set_hexpand(true);
  _previewColumnView.set_show_row_separators(true);
  _previewScrolledWindow.set_child(_previewColumnView);
  _rightPanel.append(_previewScrolledWindow);

  // Set proportional sizes (60% inputs, 40% preview)
  _leftPanel.set_hexpand(true);
  _rightPanel.set_hexpand(true);

  mainBox.append(_leftPanel);
  mainBox.append(_rightPanel);

  set_child(mainBox);
}

void NewListDialog::setupPreview()
{
  // Create FilteredTrackIdList for expression evaluation
  _previewFilteredList = std::make_unique<app::model::FilteredTrackIdList>(*_allTrackIds, *_musicLibrary);

  // Create TrackListAdapter to bridge to GTK
  _previewAdapter = std::make_shared<TrackListAdapter>(*_previewFilteredList, _rowDataProvider);

  // Create single-selection model for ColumnView
  auto selectionModel = Gtk::SingleSelection::create(_previewAdapter->getModel());
  _previewColumnView.set_model(selectionModel);

  setupPreviewColumns();
}

void NewListDialog::setupPreviewColumns()
{
  // Single column factory that formats "Title - Artist (Album)"
  auto factory = Gtk::SignalListItemFactory::create();
  factory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    label->set_ellipsize(Pango::EllipsizeMode::END);
    listItem->set_child(*label);
  });
  factory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto item = listItem->get_item();
    auto row = std::dynamic_pointer_cast<TrackRow>(item);
    auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
    if (row && label)
    {
      row->ensureLoaded();
      auto const& title = row->getTitle();
      auto const& artist = row->getArtist();
      auto const& album = row->getAlbum();
      std::string formatted;
      if (!title.empty())
      {
        formatted = title;
        if (!artist.empty())
        {
          formatted += " - " + artist;
        }
        if (!album.empty())
        {
          formatted += " (" + album + ")";
        }
      }
      else if (!artist.empty())
      {
        formatted = artist;
        if (!album.empty())
        {
          formatted += " (" + album + ")";
        }
      }
      else
      {
        formatted = "(untitled)";
      }
      label->set_text(formatted);
    }
  });

  auto column = Gtk::ColumnViewColumn::create("Track", factory);
  column->set_expand(true);
  column->set_resizable(true);
  _previewColumnView.append_column(column);
}

void NewListDialog::updatePreview()
{
  auto const& expr = _exprBox.entry().get_text();

  if (expr.empty())
  {
    _exprBox.entry().remove_css_class("error");
    _errorLabel.set_visible(false);
    _previewScrolledWindow.set_visible(true);
    _previewFilteredList->setExpression("");
    _previewFilteredList->reload();
    auto const total = _previewFilteredList->size();
    if (total == 0)
    {
      _matchCountLabel.set_markup("<i>No tracks in library</i>");
    }
    else
    {
      _matchCountLabel.set_markup(Glib::ustring::format("<i>Showing all ", total, " tracks</i>"));
    }
    return;
  }

  _previewFilteredList->setExpression(expr);
  _previewFilteredList->reload();

  if (_previewFilteredList->hasError())
  {
    // Show error state
    _exprBox.entry().add_css_class("error");
    _errorLabel.set_visible(true);
    _errorLabel.set_text("Expression error: " + _previewFilteredList->errorMessage());
    _previewScrolledWindow.set_visible(false);
    _matchCountLabel.set_markup("<i>Invalid expression</i>");
  }
  else
  {
    // Show valid state
    _exprBox.entry().remove_css_class("error");
    _errorLabel.set_visible(false);
    _previewScrolledWindow.set_visible(true);

    auto const total = _previewFilteredList->size();
    constexpr std::size_t kMaxPreview = 10;
    auto const shown = std::min(total, kMaxPreview);

    if (total == 0)
    {
      _matchCountLabel.set_markup("<i>No matches</i>");
    }
    else if (total <= kMaxPreview)
    {
      _matchCountLabel.set_markup(Glib::ustring::format("<i>Showing all ", total, " matches</i>"));
    }
    else
    {
      _matchCountLabel.set_markup(Glib::ustring::format("<i>Showing ", shown, " of ", total, " matches</i>"));
    }
  }
}

app::model::ListDraft NewListDialog::draft() const
{
  app::model::ListDraft draftData;
  draftData.kind = app::model::ListKind::Smart;
  draftData.name = _nameEntry.get_text();
  draftData.description = _descEntry.get_text();
  draftData.expression = _exprBox.entry().get_text();
  // trackIds remain empty for smart lists
  return draftData;
}
