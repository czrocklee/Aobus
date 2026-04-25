// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/SmartListDialog.h"

#include "platform/linux/ui/TrackListAdapter.h"
#include "platform/linux/ui/TrackRow.h"

#include "core/model/AllTrackIdsList.h"
#include "core/model/FilteredTrackIdList.h"
#include "core/model/SmartListEngine.h"
#include "core/model/TrackIdList.h"
#include "core/model/TrackRowDataProvider.h"

#include <rs/core/MusicLibrary.h>

#include <algorithm>

namespace app::ui
{

  namespace
  {
    std::string composeEffectiveExpression(std::string_view parent, std::string_view local)
    {
      if (parent.empty())
      {
        return std::string{local};
      }

      if (local.empty())
      {
        return std::string{parent};
      }

      return std::string{"("} + std::string{parent} + ") and (" + std::string{local} + ")";
    }

    std::string displayExpression(std::string_view expression)
    {
      if (expression.empty())
      {
        return "(none)";
      }

      return std::string{expression};
    }
  }

  SmartListDialog::SmartListDialog(Gtk::Window& parent,
                                   rs::core::MusicLibrary& musicLibrary,
                                   app::core::model::AllTrackIdsList& allTrackIds,
                                   app::core::model::TrackIdList& parentMembershipList,
                                   rs::core::ListId parentListId)
    : _exprBox{musicLibrary}
    , _musicLibrary{&musicLibrary}
    , _allTrackIds{&allTrackIds}
    , _parentMembershipList{&parentMembershipList}
    , _parentListId{parentListId}
  {
    set_title("New List");
    set_transient_for(parent);
    set_modal(true);
    setupUi();
    setupPreview();
    updatePreview();
  }

  SmartListDialog::~SmartListDialog()
  {
    _exprTimeoutConnection.disconnect();
  }

  void SmartListDialog::populate(rs::core::ListId id, rs::core::ListView const& view)
  {
    _editListId = id;
    _nameEntry.set_text(std::string(view.name()));
    _descEntry.set_text(std::string(view.description()));
    _exprBox.entry().set_text(std::string(view.filter()));
    set_title("Edit List");
    _okButton.set_label("Save");
    updateDialogState();
  }

  rs::core::ListId SmartListDialog::editListId() const
  {
    return _editListId.value_or(rs::core::ListId{0});
  }

  void SmartListDialog::setupUi()
  {
    constexpr std::int32_t kDialogWidth = 800;
    constexpr std::int32_t kDialogHeight = 500;
    constexpr std::int32_t kBoxSpacing = 8;
    constexpr std::int32_t kBoxMargin = 12;
    constexpr std::int32_t kButtonBoxSpacing = 6;
    constexpr std::int32_t kLabelMinLines = 2;

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

    auto inheritedLabel = Gtk::Label("Inherited Filter:");
    inheritedLabel.set_halign(Gtk::Align::START);
    _inheritedExprLabel.set_halign(Gtk::Align::START);
    _inheritedExprLabel.set_wrap(true);
    _inheritedExprLabel.set_lines(kLabelMinLines);
    _leftPanel.append(inheritedLabel);
    _leftPanel.append(_inheritedExprLabel);

    // Filter field
    auto exprLabel = Gtk::Label("Local Filter:");
    exprLabel.set_halign(Gtk::Align::START);
    _exprBox.entry().set_placeholder_text("Filter expression (type $, @, #, or %)");
    _exprBox.entry().signal_changed().connect(
      [this]()
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

    auto effectiveLabel = Gtk::Label("Effective Filter:");
    effectiveLabel.set_halign(Gtk::Align::START);
    _effectiveExprLabel.set_halign(Gtk::Align::START);
    _effectiveExprLabel.set_wrap(true);
    _effectiveExprLabel.set_lines(kLabelMinLines);
    _leftPanel.append(effectiveLabel);
    _leftPanel.append(_effectiveExprLabel);

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

    _nameEntry.signal_changed().connect([this]() { updateDialogState(); });

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

  void SmartListDialog::setupPreview()
  {
    // Create preview engine for expression evaluation
    _previewEngine = std::make_unique<app::core::model::SmartListEngine>(*_musicLibrary);

    setupPreviewColumns();
    rebuildPreviewSource();
  }

  void SmartListDialog::setupPreviewColumns()
  {
    // Single column factory that formats "Title - Artist (Album)"
    auto factory = Gtk::SignalListItemFactory::create();

    factory->signal_setup().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto* label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        listItem->set_child(*label);
      });

    factory->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto item = listItem->get_item();
        auto row = std::dynamic_pointer_cast<TrackRow>(item);

        if (auto label = dynamic_cast<Gtk::Label*>(listItem->get_child()); row && label)
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

  void SmartListDialog::rebuildPreviewSource()
  {
    // Use deferred execution to avoid GTK accessing freed adapter during event processing
    // Schedule model replacement to happen after current GTK event is processed
    Glib::signal_idle().connect_once(
      [this]()
      {
        // First clear the GTK model to release any held references
        auto emptySelection = Glib::RefPtr<Gtk::SelectionModel>{};
        _previewColumnView.set_model(emptySelection);

        // Now safe to reset adapter and filtered list
        _previewFilteredList.reset();
        _previewAdapter.reset();

        // Build new preview components
        std::string_view inheritedExpr;

        // Check if parent is All Tracks (passed from MainWindow as _allTrackIds)
        auto const isAllTracks = (_parentMembershipList == _allTrackIds);

        if (!isAllTracks)
        {
          auto readTxn = _musicLibrary->readTransaction();
          auto reader = _musicLibrary->lists().reader(readTxn);
          auto listView = reader.get(_parentListId);

          if (listView)
          {
            inheritedExpr = listView->filter();
          }
        }

        // Use the parent's membership list as source - this already has the inherited filter applied
        _rowDataProvider = std::make_shared<app::core::model::TrackRowDataProvider>(*_musicLibrary);

        // ALWAYS use FilteredTrackIdList for preview so we can apply the local filter
        // Even for All Tracks, we want to preview the results of the local expression
        _previewFilteredList = std::make_unique<app::core::model::FilteredTrackIdList>(
          *_parentMembershipList, *_musicLibrary, *_previewEngine);
        _previewAdapter = std::make_shared<TrackListAdapter>(*_previewFilteredList, _rowDataProvider);

        auto selectionModel = Gtk::SingleSelection::create(_previewAdapter->getModel());
        _previewColumnView.set_model(selectionModel);

        updateSourceLabels();
        updateDialogState();
      });
  }

  void SmartListDialog::updateSourceLabels()
  {
    std::string_view inheritedExpr;

    // Check if parent is All Tracks
    auto const isAllTracks = (_parentMembershipList == _allTrackIds);

    if (!isAllTracks)
    {
      auto readTxn = _musicLibrary->readTransaction();
      auto reader = _musicLibrary->lists().reader(readTxn);
      auto listView = reader.get(_parentListId);

      if (listView)
      {
        inheritedExpr = listView->filter();
      }
      else
      {
        _inheritedExprLabel.set_text("(invalid source)");
        _effectiveExprLabel.set_text("(invalid source)");
        return;
      }
    }

    _inheritedExprLabel.set_text(displayExpression(inheritedExpr));

    auto const localExpr = std::string(_exprBox.entry().get_text());
    auto const effectiveExpression = composeEffectiveExpression(inheritedExpr, localExpr);
    _effectiveExprLabel.set_text(displayExpression(effectiveExpression));
  }

  void SmartListDialog::updateDialogState()
  {
    _okButton.set_sensitive(!_nameEntry.get_text().empty() && _expressionValid);
  }

  void SmartListDialog::updatePreview()
  {
    updateSourceLabels();

    // For other lists, use FilteredTrackIdList
    
    if (!_previewFilteredList)
    {
      _expressionValid = false;
      _previewScrolledWindow.set_visible(false);
      updateDialogState();
      return;
    }

    auto const& expr = _exprBox.entry().get_text();
    auto const isAllTracks = (_parentMembershipList == _allTrackIds);

    if (expr.empty())
    {
      _exprBox.entry().remove_css_class("error");
      _errorLabel.set_visible(false);
      _previewScrolledWindow.set_visible(true);
      _previewFilteredList->setExpression("");
      _previewFilteredList->reload();
      _expressionValid = true;
      auto const total = _previewFilteredList->size();

      if (total == 0)
      {
        _matchCountLabel.set_markup(isAllTracks ? "<i>No tracks in library</i>" : "<i>No tracks in source</i>");
      }
      else
      {
        _matchCountLabel.set_markup(
          Glib::ustring::format("<i>Showing all ", total, isAllTracks ? " tracks</i>" : " source tracks</i>"));
      }
      
      updateDialogState();
      return;
    }

    _previewFilteredList->setExpression(expr);
    _previewFilteredList->reload();

    if (_previewFilteredList->hasError())
    {
      // Show error state
      _exprBox.entry().add_css_class("error");
      _errorLabel.set_visible(true);
      _errorLabel.set_text("Filter error: " + _previewFilteredList->errorMessage());
      _previewScrolledWindow.set_visible(false);
      _matchCountLabel.set_markup("<i>Invalid filter</i>");
      _expressionValid = false;
    }
    else
    {
      // Show valid state
      _exprBox.entry().remove_css_class("error");
      _errorLabel.set_visible(false);
      _previewScrolledWindow.set_visible(true);
      _expressionValid = true;

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

    updateDialogState();
  }

  app::core::model::ListDraft SmartListDialog::draft() const
  {
    auto draftData = app::core::model::ListDraft{};
    draftData.kind = app::core::model::ListKind::Smart;
    draftData.parentId = _parentListId;
    draftData.listId = editListId();
    draftData.name = _nameEntry.get_text();
    draftData.description = _descEntry.get_text();
    draftData.expression = _exprBox.entry().get_text();
    // trackIds remain empty for smart lists
    return draftData;
  }

} // namespace app::ui
