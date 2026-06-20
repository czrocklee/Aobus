// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/SmartListDialog.h"

#include "app/FormBuilder.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include "track/TrackViewPage.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/uimodel/list/SmartListEditorModel.h>

#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtk/gtktypes.h>
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
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    std::string italicMarkup(std::string_view text)
    {
      return std::format("<i>{}</i>", text);
    }
  }

  SmartListDialog::SmartListDialog(Gtk::Window& parent,
                                   rt::AppRuntime& runtime,
                                   ListId parentListId,
                                   TrackRowCache const& provider)
    : _exprBox{runtime.completion()}, _runtime{runtime}, _parentListId{parentListId}, _trackRowCache{provider}
  {
    set_title("New List");
    set_transient_for(parent);
    setupUi();
    setupPreview();
    updatePreview();
  }

  SmartListDialog::~SmartListDialog()
  {
    _exprTimeoutConnection.disconnect();
    _rebuildConnection.disconnect();
  }

  void SmartListDialog::populate(ListId id,
                                 rt::ListNode const& node,
                                 std::optional<std::string> const& optPresentationId)
  {
    _editListId = id;
    _nameEntry.set_text(node.name);
    _descEntry.set_text(node.description);
    _exprBox.entry().set_text(node.smartExpression);
    set_title("Edit List");
    _okButton->set_label("Save");

    auto const presentationIndex = uimodel::list::SmartListEditorModel::presentationIndexForId(
      optPresentationId, rt::builtinTrackPresentationPresets());
    _presentationDropDown.set_selected(static_cast<std::uint32_t>(presentationIndex));

    updateDialogState();
  }

  ListId SmartListDialog::editListId() const
  {
    return _editListId;
  }

  std::string SmartListDialog::presentationId() const
  {
    auto const selected = _presentationDropDown.get_selected();
    auto const localExpr = std::string{_exprBox.entry().get_text()};

    return uimodel::list::SmartListEditorModel::resolvePresentationId(
      selected, selected != GTK_INVALID_LIST_POSITION, localExpr, rt::builtinTrackPresentationPresets(), {});
  }

  void SmartListDialog::setLocalExpression(std::string_view expression)
  {
    _exprTimeoutConnection.disconnect();
    _exprBox.entry().set_text(std::string{expression});
    updatePreview();
  }

  void SmartListDialog::setupUi()
  {
    constexpr std::int32_t kBoxSpacing = 12;
    constexpr int kPreviewMinContentWidth = 420;
    constexpr int kPreviewMinContentHeight = 360;
    constexpr int kPreviewMaxContentWidth = 640;
    constexpr int kPreviewMaxContentHeight = 520;

    set_default_size(-1, -1);

    // Setup Actions in HeaderBar
    _cancelButton = addCancelAction("Cancel", Gtk::ResponseType::CANCEL);
    _okButton = addPrimaryAction("Create", Gtk::ResponseType::OK);
    _okButton->set_sensitive(false);

    auto* const mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing * 2);

    // Left Panel: Configuration Form
    _leftPanel.set_orientation(Gtk::Orientation::VERTICAL);
    _leftPanel.set_spacing(kBoxSpacing * 2);

    // Section 1: Metadata Card
    auto* const metaList = Gtk::make_managed<FormBoxedList>();
    _nameEntry.set_placeholder_text("List name");
    _nameEntry.signal_changed().connect([this] { updateDialogState(); });
    metaList->addEntryRow("Name", _nameEntry);

    _descEntry.set_placeholder_text("Optional description");
    metaList->addEntryRow("Description", _descEntry);
    _leftPanel.append(*metaList);

    // Section 2: Filter Card
    auto* const filterList = Gtk::make_managed<FormBoxedList>();

    // Inherited Filter
    _inheritedExprLabel.set_halign(Gtk::Align::END);
    _inheritedExprLabel.set_ellipsize(Pango::EllipsizeMode::END);
    filterList->addRow("Inherited Filter", _inheritedExprLabel);

    // Local Filter
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
    filterList->addRow("Local Filter", _exprBox);

    // Effective Filter
    _effectiveExprLabel.set_halign(Gtk::Align::END);
    _effectiveExprLabel.set_ellipsize(Pango::EllipsizeMode::END);
    filterList->addRow("Effective Filter", _effectiveExprLabel);

    _leftPanel.append(*filterList);

    // Section 3: Presentation Preference
    auto* const presList = Gtk::make_managed<FormBoxedList>();
    auto stringListPtr = Gtk::StringList::create();
    stringListPtr->append("Auto");

    for (auto const& preset : rt::builtinTrackPresentationPresets())
    {
      stringListPtr->append(std::string{preset.label});
    }

    _presentationDropDown.set_model(stringListPtr);
    _presentationDropDown.set_valign(Gtk::Align::CENTER);
    _presentationDropDown.set_halign(Gtk::Align::END);
    _presentationDropDown.property_selected().signal_changed().connect([this] { updatePreview(); });
    presList->addRow("Presentation", _presentationDropDown);
    _leftPanel.append(*presList);

    // Error Label (Global for the left panel)
    _errorLabel.set_visible(false);
    _errorLabel.set_wrap(true);
    _errorLabel.set_halign(Gtk::Align::START);
    _errorLabel.add_css_class("ao-layout-error"); // Reuse existing error style if appropriate
    _leftPanel.append(_errorLabel);

    // Right Panel: Preview
    _rightPanel.set_orientation(Gtk::Orientation::VERTICAL);
    _rightPanel.set_spacing(kBoxSpacing);
    _rightPanel.set_hexpand(true);
    _rightPanel.set_vexpand(true);

    // Wrap Preview in a Card-like structure if needed, or just standard spacing
    auto* const previewHeaderBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing);
    auto* const previewLabel = Gtk::make_managed<Gtk::Label>("Preview");
    previewLabel->add_css_class("ao-section-header");
    previewLabel->set_halign(Gtk::Align::START);
    previewHeaderBox->append(*previewLabel);

    _matchCountLabel.set_halign(Gtk::Align::END);
    _matchCountLabel.set_hexpand(true);
    _matchCountLabel.set_markup("<span alpha='50%'><i>Waiting for filter...</i></span>");
    previewHeaderBox->append(_matchCountLabel);
    _rightPanel.append(*previewHeaderBox);

    _previewScrolledWindow.set_vexpand(true);
    _previewScrolledWindow.set_hexpand(true);
    _previewScrolledWindow.set_propagate_natural_width(true);
    _previewScrolledWindow.set_propagate_natural_height(true);
    _previewScrolledWindow.set_min_content_width(kPreviewMinContentWidth);
    _previewScrolledWindow.set_min_content_height(kPreviewMinContentHeight);
    _previewScrolledWindow.set_max_content_width(kPreviewMaxContentWidth);
    _previewScrolledWindow.set_max_content_height(kPreviewMaxContentHeight);
    _previewScrolledWindow.add_css_class("ao-content-shell-modern"); // Reuse shell styling
    _previewColumnView.set_show_row_separators(true);
    _previewScrolledWindow.set_child(_previewColumnView);
    _rightPanel.append(_previewScrolledWindow);

    mainBox->append(_leftPanel);
    mainBox->append(_rightPanel);
    _leftPanel.set_hexpand(true);

    setContentWidget(*mainBox);
  }

  void SmartListDialog::setupPreview()
  {
    _previewEnginePtr = std::make_unique<rt::SmartListEvaluator>(_runtime.musicLibrary());
    setupPreviewColumns();
    rebuildPreviewSource();
  }

  void SmartListDialog::setupPreviewColumns()
  {
    auto factoryPtr = Gtk::SignalListItemFactory::create();

    factoryPtr->signal_setup().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto* const label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        listItem->set_child(*label);
      });

    factoryPtr->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto const itemPtr = listItem->get_item();
        auto rowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr);

        if (auto* const label = dynamic_cast<Gtk::Label*>(listItem->get_child()); rowPtr && label)
        {
          auto const* title = rowPtr->stringField(rt::TrackField::Title);
          auto const* artist = rowPtr->stringField(rt::TrackField::Artist);
          auto const* album = rowPtr->stringField(rt::TrackField::Album);
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

    auto columnPtr = Gtk::ColumnViewColumn::create("Track", factoryPtr);
    columnPtr->set_expand(true);
    columnPtr->set_resizable(true);
    _previewColumnView.append_column(columnPtr);
  }

  void SmartListDialog::rebuildPreviewSource()
  {
    _rebuildConnection.disconnect();
    _rebuildConnection = Glib::signal_idle().connect(
      [this]
      {
        auto emptySelectionPtr = Glib::RefPtr<Gtk::SelectionModel>{};
        _previewColumnView.set_model(emptySelectionPtr);

        _previewFilteredListPtr.reset();
        _previewModelPtr.reset();

        auto& parentSource = _runtime.sources().sourceFor(_parentListId);

        _previewFilteredListPtr =
          std::make_unique<rt::SmartListSource>(parentSource, _runtime.musicLibrary(), *_previewEnginePtr);

        auto projPtr = std::make_shared<rt::TrackListProjection>(
          rt::kInvalidViewId, *_previewFilteredListPtr, _runtime.musicLibrary());

        _previewModelPtr = TrackListModel::create(_trackRowCache);
        _previewModelPtr->bindProjection(std::move(projPtr));

        auto selectionModelPtr = Gtk::SingleSelection::create(_previewModelPtr);
        _previewColumnView.set_model(selectionModelPtr);

        updateSourceLabels();
        updateDialogState();

        return false;
      });
  }

  void SmartListDialog::updateSourceLabels()
  {
    auto inheritedExpr = std::string{};
    auto const isAllTracks = (_parentListId == rt::kAllTracksListId || _parentListId == kInvalidListId);

    if (!isAllTracks)
    {
      auto scope = _runtime.library().reader();

      if (auto optNode = scope.listNode(_parentListId); optNode)
      {
        inheritedExpr = optNode->smartExpression;
      }
      else
      {
        _inheritedExprLabel.set_text("(invalid source)");
        _effectiveExprLabel.set_text("(invalid source)");
        return;
      }
    }

    _inheritedExprLabel.set_text(uimodel::list::SmartListEditorModel::displayExpression(inheritedExpr));

    auto const localExpr = std::string{_exprBox.entry().get_text()};
    auto const effectiveExpression =
      ao::uimodel::list::SmartListEditorModel::composeEffectiveExpression(inheritedExpr, localExpr);
    _effectiveExprLabel.set_text(uimodel::list::SmartListEditorModel::displayExpression(effectiveExpression));
  }

  void SmartListDialog::updateDialogState()
  {
    auto const status =
      ao::uimodel::list::SmartListEditorModel::dialogStatus(_expressionValid, _previewFilteredListPtr != nullptr);

    _okButton->set_sensitive(ao::uimodel::list::SmartListEditorModel::canSubmit(_nameEntry.get_text().raw(), status));
  }

  void SmartListDialog::updatePreview()
  {
    updateSourceLabels();

    if (!_previewFilteredListPtr)
    {
      auto const state = ao::uimodel::list::SmartListEditorModel::previewState(ao::uimodel::list::SmartListPreviewInput{
        .name = _nameEntry.get_text().raw(),
        .localExpression = _exprBox.entry().get_text().raw(),
        .hasPreviewSource = false,
        .hasError = false,
        .errorMessage = "",
        .matchCount = 0,
        .isAllTracks = false,
      });
      _expressionValid = state.expressionValid;
      _previewScrolledWindow.set_visible(false);
      updateDialogState();
      return;
    }

    auto const expr = std::string{_exprBox.entry().get_text()};
    auto const isAllTracks = (_parentListId == rt::kAllTracksListId || _parentListId == kInvalidListId);

    _previewFilteredListPtr->setExpression(expr);
    _previewFilteredListPtr->reload();

    auto const hasError = _previewFilteredListPtr->hasError();
    auto const optError = _previewFilteredListPtr->error();
    auto const errorMessage = optError ? optError->message : std::string{};
    auto const matchCount = _previewFilteredListPtr->size();

    auto const state = ao::uimodel::list::SmartListEditorModel::previewState(ao::uimodel::list::SmartListPreviewInput{
      .name = _nameEntry.get_text().raw(),
      .localExpression = expr,
      .hasPreviewSource = true,
      .hasError = hasError,
      .errorMessage = errorMessage,
      .matchCount = matchCount,
      .isAllTracks = isAllTracks,
    });

    _matchCountLabel.set_markup(italicMarkup(state.previewStatusText));

    if (state.queryInvalid)
    {
      _exprBox.entry().add_css_class("ao-query-invalid");
    }
    else
    {
      _exprBox.entry().remove_css_class("ao-query-invalid");
    }

    _errorLabel.set_visible(state.errorVisible);
    _errorLabel.set_text(state.errorText);
    _previewScrolledWindow.set_visible(state.previewVisible);
    _expressionValid = state.expressionValid;

    updateDialogState();
  }

  rt::LibraryWriter::ListDraft SmartListDialog::draft() const
  {
    return ao::uimodel::list::SmartListEditorModel::createDraft(
      _parentListId, _editListId, _nameEntry.get_text(), _descEntry.get_text(), _exprBox.entry().get_text());
  }
} // namespace ao::gtk
