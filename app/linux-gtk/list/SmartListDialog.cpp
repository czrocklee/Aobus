// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/SmartListDialog.h"

#include "ao/Type.h"
#include "ao/library/ListStore.h"
#include "ao/library/ListView.h"
#include "ao/library/MusicLibrary.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include "track/TrackViewPage.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/SmartListEvaluator.h>
#include <ao/rt/SmartListSource.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackListProjection.h>
#include <ao/rt/TrackSource.h>
#include <ao/uimodel/list/SmartListEditorModel.h>

#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace std::string_literals;

namespace ao::gtk
{
  namespace
  {
    std::string displayExpression(std::string_view expression)
    {
      return expression.empty() ? "(none)" : std::string{expression};
    }

    std::string formatPreviewStatus(uimodel::list::SmartListStatus status,
                                    std::size_t count,
                                    bool isAllTracks,
                                    bool localEmpty)
    {
      using Status = uimodel::list::SmartListStatus;

      if (localEmpty)
      {
        if (count == 0)
        {
          return isAllTracks ? "<i>No tracks in library</i>" : "<i>No tracks in source</i>";
        }

        return std::format("<i>Showing all {} {}</i>", count, isAllTracks ? "tracks" : "source tracks");
      }

      switch (status)
      {
        case Status::InvalidExpression: return "<i>Invalid filter</i>";
        case Status::EmptySource: return "<i>No tracks in source</i>";
        case Status::Valid:
        {
          if (count == 0)
          {
            return "<i>No matches</i>";
          }

          constexpr std::size_t kMaxPreview = 10;

          if (count <= kMaxPreview)
          {
            return std::format("<i>Showing all {} matches</i>", count);
          }

          return std::format("<i>Showing {} of {} matches</i>", kMaxPreview, count);
        }
      }

      return "";
    }
  }

  SmartListDialog::SmartListDialog(Gtk::Window& parent,
                                   rt::AppRuntime& runtime,
                                   ListId parentListId,
                                   TrackRowCache const& provider)
    : _exprBox{runtime.musicLibrary()}, _runtime{runtime}, _parentListId{parentListId}, _trackRowCache{provider}
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
    _rebuildConnection.disconnect();
  }

  void SmartListDialog::populate(ListId id, library::ListView const& view)
  {
    _editListId = id;
    _nameEntry.set_text(std::string{view.name()});
    _descEntry.set_text(std::string{view.description()});
    _exprBox.entry().set_text(std::string{view.filter()});
    set_title("Edit List");
    _okButton.set_label("Save");
    updateDialogState();
  }

  ListId SmartListDialog::editListId() const
  {
    return _editListId;
  }

  void SmartListDialog::setLocalExpression(std::string_view expression)
  {
    _exprTimeoutConnection.disconnect();
    _exprBox.entry().set_text(std::string{expression});
    updatePreview();
  }

  void SmartListDialog::setupUi()
  {
    constexpr std::int32_t kDialogWidth = 800;
    constexpr std::int32_t kDialogHeight = 500;
    constexpr std::int32_t kBoxSpacing = 8;
    constexpr std::int32_t kButtonBoxSpacing = 6;
    constexpr std::int32_t kLabelMinLines = 2;

    set_default_size(kDialogWidth, kDialogHeight);

    auto* const mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing * 2);
    mainBox->add_css_class("ao-dialog-content");

    _leftPanel.set_orientation(Gtk::Orientation::VERTICAL);
    _leftPanel.set_spacing(kBoxSpacing);

    auto* const nameLabel = Gtk::make_managed<Gtk::Label>("Name:");
    nameLabel->set_halign(Gtk::Align::START);
    _nameEntry.set_placeholder_text("List name");
    _leftPanel.append(*nameLabel);
    _leftPanel.append(_nameEntry);

    auto* const descLabel = Gtk::make_managed<Gtk::Label>("Description:");
    descLabel->set_halign(Gtk::Align::START);
    _descEntry.set_placeholder_text("Optional description");
    _leftPanel.append(*descLabel);
    _leftPanel.append(_descEntry);

    auto* const inheritedLabel = Gtk::make_managed<Gtk::Label>("Inherited Filter:");
    inheritedLabel->set_halign(Gtk::Align::START);
    _inheritedExprLabel.set_halign(Gtk::Align::START);
    _inheritedExprLabel.set_wrap(true);
    _inheritedExprLabel.set_lines(kLabelMinLines);
    _leftPanel.append(*inheritedLabel);
    _leftPanel.append(_inheritedExprLabel);

    auto* const exprLabel = Gtk::make_managed<Gtk::Label>("Local Filter:");
    exprLabel->set_halign(Gtk::Align::START);
    _exprBox.entry().set_placeholder_text("Filter expression (type $, @, #, or %)");
    _exprBox.entry().signal_changed().connect(
      [this]
      {
        _exprTimeoutConnection.disconnect();
        _exprTimeoutConnection = Glib::signal_timeout().connect(
          [this]
          {
            updatePreview();
            return false;
          },
          100);
      });
    _leftPanel.append(*exprLabel);
    _leftPanel.append(_exprBox);

    auto* const effectiveLabel = Gtk::make_managed<Gtk::Label>("Effective Filter:");
    effectiveLabel->set_halign(Gtk::Align::START);
    _effectiveExprLabel.set_halign(Gtk::Align::START);
    _effectiveExprLabel.set_wrap(true);
    _effectiveExprLabel.set_lines(kLabelMinLines);
    _leftPanel.append(*effectiveLabel);
    _leftPanel.append(_effectiveExprLabel);

    _errorLabel.set_visible(false);
    _errorLabel.set_wrap(true);
    _errorLabel.set_halign(Gtk::Align::START);
    _leftPanel.append(_errorLabel);

    auto* const buttonBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kButtonBoxSpacing);
    buttonBox->set_halign(Gtk::Align::END);
    buttonBox->add_css_class("ao-dialog-actions");

    _cancelButton.set_label("Cancel");
    _cancelButton.signal_clicked().connect([this] { response(Gtk::ResponseType::CANCEL); });

    _okButton.set_label("Create");
    _okButton.set_sensitive(false);
    _okButton.signal_clicked().connect([this] { response(Gtk::ResponseType::OK); });

    _nameEntry.signal_changed().connect([this] { updateDialogState(); });

    buttonBox->append(_cancelButton);
    buttonBox->append(_okButton);
    _leftPanel.append(*buttonBox);

    _rightPanel.set_orientation(Gtk::Orientation::VERTICAL);
    _rightPanel.set_spacing(kBoxSpacing);
    _rightPanel.set_hexpand(true);
    _rightPanel.set_vexpand(true);

    auto* const previewLabel = Gtk::make_managed<Gtk::Label>("Preview:");
    previewLabel->set_halign(Gtk::Align::START);
    _rightPanel.append(*previewLabel);

    _matchCountLabel.set_halign(Gtk::Align::START);
    _matchCountLabel.set_markup("<i>Enter an expression to see matches</i>");
    _rightPanel.append(_matchCountLabel);

    _previewScrolledWindow.set_vexpand(true);
    _previewScrolledWindow.set_hexpand(true);
    _previewColumnView.set_show_row_separators(true);
    _previewScrolledWindow.set_child(_previewColumnView);
    _rightPanel.append(_previewScrolledWindow);

    _leftPanel.set_hexpand(true);
    _rightPanel.set_hexpand(true);

    mainBox->append(_leftPanel);
    mainBox->append(_rightPanel);

    set_child(*mainBox);
  }

  void SmartListDialog::setupPreview()
  {
    _previewEngine = std::make_unique<rt::SmartListEvaluator>(_runtime.musicLibrary());
    setupPreviewColumns();
    rebuildPreviewSource();
  }

  void SmartListDialog::setupPreviewColumns()
  {
    auto factory = Gtk::SignalListItemFactory::create();

    factory->signal_setup().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto* const label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        listItem->set_child(*label);
      });

    factory->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto const item = listItem->get_item();
        auto row = std::dynamic_pointer_cast<TrackRowObject>(item);

        if (auto* const label = dynamic_cast<Gtk::Label*>(listItem->get_child()); row && label)
        {
          auto const* title = row->stringField(rt::TrackField::Title);
          auto const* artist = row->stringField(rt::TrackField::Artist);
          auto const* album = row->stringField(rt::TrackField::Album);
          auto formatted = std::string{"(untitled)"};

          if (title != nullptr && !title->empty())
          {
            formatted = title->raw();

            if (artist != nullptr && !artist->empty())
            {
              formatted = std::format("{} - {}", formatted, artist->raw());
            }

            if (album != nullptr && !album->empty())
            {
              formatted = std::format("{} ({})", formatted, album->raw());
            }
          }
          else if (artist != nullptr && !artist->empty())
          {
            formatted = artist->raw();

            if (album != nullptr && !album->empty())
            {
              formatted = std::format("{} ({})", formatted, album->raw());
            }
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
    _rebuildConnection.disconnect();
    _rebuildConnection = Glib::signal_idle().connect(
      [this]
      {
        auto emptySelection = Glib::RefPtr<Gtk::SelectionModel>{};
        _previewColumnView.set_model(emptySelection);

        _previewFilteredList.reset();
        _previewModel.reset();

        auto& parentSource = _runtime.sources().sourceFor(_parentListId);

        _previewFilteredList =
          std::make_unique<rt::SmartListSource>(parentSource, _runtime.musicLibrary(), *_previewEngine);

        auto proj =
          std::make_shared<rt::TrackListProjection>(rt::kInvalidViewId, *_previewFilteredList, _runtime.musicLibrary());

        _previewModel = TrackListModel::create(_trackRowCache);
        _previewModel->bindProjection(std::move(proj));

        auto selectionModel = Gtk::SingleSelection::create(_previewModel);
        _previewColumnView.set_model(selectionModel);

        updateSourceLabels();
        updateDialogState();

        return false;
      });
  }

  void SmartListDialog::updateSourceLabels()
  {
    auto inheritedExpr = std::string_view{};
    auto const isAllTracks = (_parentListId == rt::kAllTracksListId || _parentListId == kInvalidListId);

    if (!isAllTracks)
    {
      auto readTxn = _runtime.musicLibrary().readTransaction();
      auto reader = _runtime.musicLibrary().lists().reader(readTxn);

      if (auto optView = reader.get(_parentListId); optView)
      {
        inheritedExpr = optView->filter();
      }
      else
      {
        _inheritedExprLabel.set_text("(invalid source)");
        _effectiveExprLabel.set_text("(invalid source)");
        return;
      }
    }

    _inheritedExprLabel.set_text(displayExpression(inheritedExpr));

    auto const localExpr = std::string{_exprBox.entry().get_text()};
    auto const effectiveExpression =
      ao::uimodel::list::SmartListEditorModel::composeEffectiveExpression(inheritedExpr, localExpr);
    _effectiveExprLabel.set_text(displayExpression(effectiveExpression));
  }

  void SmartListDialog::updateDialogState()
  {
    using ao::uimodel::list::SmartListStatus;

    auto const status = [this]
    {
      if (!_expressionValid)
      {
        return SmartListStatus::InvalidExpression;
      }

      if (!_previewFilteredList)
      {
        return SmartListStatus::EmptySource;
      }

      return SmartListStatus::Valid;
    }();

    _okButton.set_sensitive(ao::uimodel::list::SmartListEditorModel::canSubmit(_nameEntry.get_text().raw(), status));
  }

  void SmartListDialog::updatePreview()
  {
    updateSourceLabels();

    if (!_previewFilteredList)
    {
      _expressionValid = false;
      _previewScrolledWindow.set_visible(false);
      updateDialogState();
      return;
    }

    auto const expr = std::string{_exprBox.entry().get_text()};
    auto const isAllTracks = (_parentListId == rt::kAllTracksListId || _parentListId == kInvalidListId);

    _previewFilteredList->setExpression(expr);
    _previewFilteredList->reload();

    auto const hasError = _previewFilteredList->hasError();
    auto const optError = _previewFilteredList->error();
    auto const errorMessage = optError ? optError->message : std::string{};
    auto const matchCount = _previewFilteredList->size();

    auto const status =
      hasError ? ao::uimodel::list::SmartListStatus::InvalidExpression : ao::uimodel::list::SmartListStatus::Valid;

    _matchCountLabel.set_markup(formatPreviewStatus(status, matchCount, isAllTracks, expr.empty()));

    if (hasError && !expr.empty())
    {
      _exprBox.entry().add_css_class("error");
      _errorLabel.set_visible(true);
      _errorLabel.set_text("Filter error: " + errorMessage);
      _previewScrolledWindow.set_visible(false);
      _expressionValid = false;
    }
    else
    {
      _exprBox.entry().remove_css_class("error");
      _errorLabel.set_visible(false);
      _previewScrolledWindow.set_visible(true);
      _expressionValid = true;
    }

    updateDialogState();
  }

  rt::LibraryMutationService::ListDraft SmartListDialog::draft() const
  {
    return ao::uimodel::list::SmartListEditorModel::createDraft(
      _parentListId, _editListId, _nameEntry.get_text(), _descEntry.get_text(), _exprBox.entry().get_text());
  }
} // namespace ao::gtk
