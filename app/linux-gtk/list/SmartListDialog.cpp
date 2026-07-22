// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/SmartListDialog.h"

#include "app/FormBuilder.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include "track/TrackViewPage.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/list/SmartListDraft.h>
#include <ao/uimodel/library/list/SmartListEditorViewState.h>
#include <ao/uimodel/library/list/SmartListExpression.h>
#include <ao/uimodel/library/list/SmartListPreview.h>
#include <ao/uimodel/library/list/SmartListTrackPresentationResolver.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

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
    configureForParent(parent);
    buildUi();
    buildPreview();
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

    auto const presentationIndex =
      uimodel::resolveSmartListTrackPresentationIndex(optPresentationId, rt::builtinTrackPresentationPresets());
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

    return uimodel::resolveSmartListTrackPresentationId(
      selected, selected != GTK_INVALID_LIST_POSITION, localExpr, rt::builtinTrackPresentationPresets(), {});
  }

  void SmartListDialog::setLocalExpression(std::string_view expression)
  {
    _exprTimeoutConnection.disconnect();
    _exprBox.entry().set_text(std::string{expression});
    updatePreview();
  }

  void SmartListDialog::showError(std::string_view message)
  {
    _errorLabel.set_text(std::string{message});
    _errorLabel.set_visible(!message.empty());
  }

  void SmartListDialog::buildUi()
  {
    constexpr std::int32_t kBoxSpacing = 12;
    constexpr int kPreviewMinContentWidth = 420;
    constexpr int kPreviewMinContentHeight = 360;
    constexpr int kPreviewMaxContentWidth = 640;
    constexpr int kPreviewMaxContentHeight = 520;
    constexpr int kConfigPanelWidth = 360;

    set_default_size(-1, -1);

    _cancelButton = addCancelAction("Cancel", Gtk::ResponseType::CANCEL);
    _okButton = addPrimaryAction("Create", Gtk::ResponseType::OK);
    _okButton->set_sensitive(false);

    auto* const mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing * 2);
    mainBox->add_css_class("ao-dialog-two-pane");
    mainBox->set_hexpand(true);
    mainBox->set_vexpand(true);

    _leftPanel.set_orientation(Gtk::Orientation::VERTICAL);
    _leftPanel.set_spacing(kBoxSpacing * 2);
    _leftPanel.set_size_request(kConfigPanelWidth, -1);
    _leftPanel.set_hexpand(false);
    _leftPanel.set_vexpand(true);
    _leftPanel.add_css_class("ao-dialog-config-pane");

    auto* const detailsList = Gtk::make_managed<FormBoxedList>();
    _nameEntry.set_placeholder_text("List name");
    _nameEntry.signal_changed().connect([this] { updateDialogState(); });
    detailsList->addEntryRow("Name", _nameEntry);

    _descEntry.set_placeholder_text("Optional description");
    detailsList->addEntryRow("Description", _descEntry);
    _leftPanel.append(*detailsList);

    auto* const filterList = Gtk::make_managed<FormBoxedList>();

    _inheritedExprLabel.set_halign(Gtk::Align::END);
    _inheritedExprLabel.set_ellipsize(Pango::EllipsizeMode::END);
    filterList->addRow("Inherited Filter", _inheritedExprLabel);

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

    _effectiveExprLabel.set_halign(Gtk::Align::END);
    _effectiveExprLabel.set_ellipsize(Pango::EllipsizeMode::END);
    filterList->addRow("Effective Filter", _effectiveExprLabel);

    _leftPanel.append(*filterList);

    auto* const presList = Gtk::make_managed<FormBoxedList>();
    auto stringListPtr = Gtk::StringList::create();
    stringListPtr->append("Auto");

    for (auto const& preset : rt::builtinTrackPresentationPresets())
    {
      auto const optText = uimodel::PresentationTextCatalog{}.builtinTrackPresentation(preset.spec.id);
      stringListPtr->append(optText ? std::string{optText->label} : preset.spec.id);
    }

    _presentationDropDown.set_model(stringListPtr);
    _presentationDropDown.set_valign(Gtk::Align::CENTER);
    _presentationDropDown.set_halign(Gtk::Align::END);
    _presentationDropDown.property_selected().signal_changed().connect([this] { updatePreview(); });
    presList->addRow("Presentation", _presentationDropDown);
    _leftPanel.append(*presList);

    _errorLabel.set_visible(false);
    _errorLabel.set_wrap(true);
    _errorLabel.set_halign(Gtk::Align::START);
    _errorLabel.add_css_class("ao-layout-error"); // Reuse existing error style if appropriate
    _leftPanel.append(_errorLabel);

    _rightPanel.set_orientation(Gtk::Orientation::VERTICAL);
    _rightPanel.set_spacing(kBoxSpacing);
    _rightPanel.set_hexpand(true);
    _rightPanel.set_vexpand(true);
    _rightPanel.add_css_class("ao-dialog-preview-pane");

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

    setContentWidget(*mainBox);
  }

  void SmartListDialog::buildPreview()
  {
    _previewEnginePtr = std::make_unique<rt::SmartListEvaluator>(_runtime.musicLibrary());
    configurePreviewColumns();
    rebuildPreviewSource();
  }

  void SmartListDialog::configurePreviewColumns()
  {
    auto factoryPtr = Gtk::SignalListItemFactory::create();

    factoryPtr->signal_setup().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
      {
        auto* const label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        listItemPtr->set_child(*label);
      });

    factoryPtr->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
      {
        auto const itemPtr = listItemPtr->get_item();
        auto rowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr);

        if (auto* const label = dynamic_cast<Gtk::Label*>(listItemPtr->get_child()); rowPtr && label)
        {
          auto const* title = rowPtr->stringField(rt::TrackField::Title);
          auto const* artist = rowPtr->stringField(rt::TrackField::Artist);
          auto const* album = rowPtr->stringField(rt::TrackField::Album);
          auto const titleText = title != nullptr ? std::string_view{title->raw()} : std::string_view{};
          auto const artistText = artist != nullptr ? std::string_view{artist->raw()} : std::string_view{};
          auto const albumText = album != nullptr ? std::string_view{album->raw()} : std::string_view{};

          label->set_text(uimodel::formatSmartListPreviewTrackLabel(titleText, artistText, albumText));
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

        auto parentResult = _runtime.sources().acquire(_parentListId);

        if (!parentResult)
        {
          APP_LOG_ERROR(
            "Cannot build smart-list preview for source {}: {}", _parentListId, parentResult.error().message);
          updateSourceLabels();
          updateDialogState();
          showError(parentResult.error().message);
          return false;
        }

        _previewFilteredListPtr = std::make_shared<rt::SmartListSource>(*parentResult, *_previewEnginePtr);

        auto projPtr = std::make_shared<rt::LiveTrackListProjection>(rt::kInvalidViewId,
                                                                     rt::TrackSourceLease{_previewFilteredListPtr},
                                                                     _runtime.musicLibrary(),
                                                                     rt::TrackOrderSpec{});

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

    _inheritedExprLabel.set_text(uimodel::formatSmartListExpressionDisplayText(inheritedExpr));

    auto const localExpr = std::string{_exprBox.entry().get_text()};
    auto const effectiveExpression = ao::uimodel::combineSmartListEffectiveExpression(inheritedExpr, localExpr);
    _effectiveExprLabel.set_text(uimodel::formatSmartListExpressionDisplayText(effectiveExpression));
  }

  void SmartListDialog::updateDialogState()
  {
    auto const status = ao::uimodel::deriveSmartListPreviewStatus(_expressionValid, _previewFilteredListPtr != nullptr);

    _okButton->set_sensitive(ao::uimodel::canSubmitSmartListDraft(_nameEntry.get_text().raw(), status));
  }

  void SmartListDialog::updatePreview()
  {
    updateSourceLabels();

    if (!_previewFilteredListPtr)
    {
      auto const state = ao::uimodel::makeSmartListEditorViewState(ao::uimodel::SmartListPreviewState{
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

    auto const state = ao::uimodel::makeSmartListEditorViewState(ao::uimodel::SmartListPreviewState{
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

  rt::LibraryListDraft SmartListDialog::draft() const
  {
    return ao::uimodel::makeSmartListDraft(
      _parentListId, _editListId, _nameEntry.get_text(), _descEntry.get_text(), _exprBox.entry().get_text());
  }
} // namespace ao::gtk
