// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/SmartListDialog.h"

#include "app/FormBuilder.h"
#include "common/AccessibleLabel.h"
#include "i18n/GtkText.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WritableTagList.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/uimodel/library/list/SmartListEditing.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <glibmm/binding.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtk/gtktypes.h>
#include <gtkmm/accessible.h>
#include <gtkmm/box.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/slicelistmodel.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/stringobject.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    std::string italicMarkup(std::string_view text)
    {
      return std::format("<i>{}</i>", Glib::Markup::escape_text(std::string{text}).raw());
    }
  }

  SmartListDialog::SmartListDialog(Gtk::Window& parent,
                                   rt::Library& library,
                                   rt::ViewService& views,
                                   rt::TrackSourceCache& sources,
                                   rt::CompletionService& completion,
                                   std::span<rt::CustomTrackPresentationPreset const> customPresets,
                                   i18n::MessageCatalog textCatalog,
                                   ListId parentListId,
                                   TrackRowCache const& provider)
    : _exprBox{completion, textCatalog}
    , _library{library}
    , _views{views}
    , _sources{sources}
    , _textCatalog{textCatalog}
    , _customPresets{customPresets.begin(), customPresets.end()}
    , _parentListId{parentListId}
    , _trackRowCache{provider}
  {
    set_title(gtkText(_textCatalog, i18n::MessageId::GtkSmartListNewTitle));
    configureForParent(parent);
    buildUi();
    buildPreview();
    refreshDraftPreview();
  }

  SmartListDialog::~SmartListDialog()
  {
    _presentationCallbacks.close();
    _nameChangedConnection.disconnect();
    _descriptionChangedConnection.disconnect();
    _expressionChangedConnection.disconnect();
    _exprTimeoutConnection.disconnect();
    _previewSetupConnection.disconnect();
    _previewRefreshConnection.disconnect();
    _previewSourceSubscription.reset();
    _previewColumnView.set_model({});

    if (_previewModelPtr)
    {
      _previewModelPtr->clearProjection();
    }
  }

  void SmartListDialog::populate(ListId id,
                                 rt::ListNode const& node,
                                 std::optional<std::string> const& optPresentationId)
  {
    _submissionError.clear();
    _editListId = id;
    _nameEntry.set_text(node.name);
    _descEntry.set_text(node.description);
    _exprBox.entry().set_text(node.expression);
    set_title(gtkText(_textCatalog, i18n::MessageId::GtkSmartListEditTitle));
    _okButton->set_label(gtkText(_textCatalog, i18n::MessageId::GtkCommonSave));

    // Repeated population is reset-safe: an unavailable option installed by
    // a previous populate() is dropped before this stored id is resolved.
    removeUnavailablePresentationOption();

    auto const presentationIndex = uimodel::resolveSmartListTrackPresentationIndex(
      optPresentationId, rt::builtinTrackPresentationPresets(), _customPresets);

    // A nonempty stored id that resolves to Auto is unknown to the option
    // snapshot: it stays the user's saved intent as an explicit unavailable
    // option instead of silently becoming Auto. An empty stored id is Auto.
    if (optPresentationId && !optPresentationId->empty() &&
        presentationIndex == uimodel::kSmartListAutoTrackPresentationIndex)
    {
      installUnavailablePresentationOption(*optPresentationId);
    }
    else
    {
      _presentationDropDown.set_selected(static_cast<std::uint32_t>(presentationIndex));
    }

    refreshDraftPreview();
  }

  ListId SmartListDialog::editListId() const
  {
    return _editListId;
  }

  std::string SmartListDialog::presentationId() const
  {
    // The unavailable option is still selected: the retained opaque id is
    // the user's preference, so it round-trips unchanged. Any intentionally
    // selected row resolves through the normal option order instead.
    if (_optUnavailablePresentationId && _presentationDropDown.get_selected() == unavailablePresentationRow())
    {
      return *_optUnavailablePresentationId;
    }

    auto const selected = _presentationDropDown.get_selected();

    return uimodel::resolveSmartListTrackPresentationId(
      selected, selected != GTK_INVALID_LIST_POSITION, rt::builtinTrackPresentationPresets(), _customPresets);
  }

  void SmartListDialog::configurePlaylistTemplate(std::string_view const initialName, std::string_view const initialTag)
  {
    _submissionError.clear();
    _playlistTemplate = true;
    set_title(gtkText(_textCatalog, i18n::MessageId::GtkSmartListNewPlaylistTitle));
    _membershipTagRow->set_visible(true);
    _exprBox.entry().set_editable(false);
    _nameEntry.set_text(std::string{initialName});

    auto const tag = initialTag.empty() ? initialName : initialTag;
    _syncingMembershipTag = true;
    _membershipTagEntry.set_text(std::string{tag});
    _syncingMembershipTag = false;
    _membershipTagEdited = !initialTag.empty();

    auto const presets = rt::builtinTrackPresentationPresets();

    for (std::size_t index = 0; index < presets.size(); ++index)
    {
      if (presets[index].spec.id == "list-order")
      {
        _presentationDropDown.set_selected(static_cast<std::uint32_t>(index + 1));
        break;
      }
    }

    updatePlaylistExpression();
  }

  void SmartListDialog::setLocalExpression(std::string_view expression)
  {
    if (_exprBox.entry().get_text().raw() != expression)
    {
      _exprBox.entry().set_text(std::string{expression});
    }

    refreshDraftPreview();
  }

  void SmartListDialog::showError(std::string_view message)
  {
    _submissionError.assign(message);
    updateErrorLabel(editorViewState());
  }

  bool SmartListDialog::tryBeginSubmission()
  {
    if (_submissionPending)
    {
      return false;
    }

    _submissionError.clear();
    refreshDraftPreview();

    // The refreshed state includes the source, name, and playlist-tag gates.
    if (!_okButton->get_sensitive())
    {
      return false;
    }

    _submissionPending = true;
    _okButton->set_sensitive(false);
    return true;
  }

  void SmartListDialog::completeSubmission()
  {
    _submissionPending = false;
    updateDialogState();
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

    _cancelButton = addCancelAction(gtkText(_textCatalog, i18n::MessageId::GtkCommonCancel), Gtk::ResponseType::CANCEL);
    _okButton = addPrimaryAction(gtkText(_textCatalog, i18n::MessageId::GtkCommonCreate), Gtk::ResponseType::OK);
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
    _nameEntry.set_placeholder_text(gtkText(_textCatalog, i18n::MessageId::GtkSmartListNamePlaceholder));
    _nameChangedConnection = _nameEntry.signal_changed().connect(
      [this]
      {
        dismissSubmissionError();
        updatePlaylistTagFromName();
        updateDialogState();
      });
    detailsList->addEntryRow(gtkText(_textCatalog, i18n::MessageId::GtkSmartListName), _nameEntry);

    _descEntry.set_placeholder_text(gtkText(_textCatalog, i18n::MessageId::GtkSmartListDescriptionPlaceholder));
    _descriptionChangedConnection = _descEntry.signal_changed().connect([this] { dismissSubmissionError(); });
    detailsList->addEntryRow(gtkText(_textCatalog, i18n::MessageId::GtkSmartListDescription), _descEntry);

    _membershipTagEntry.set_placeholder_text(
      gtkText(_textCatalog, i18n::MessageId::GtkSmartListMembershipTagPlaceholder));
    _membershipTagEntry.signal_changed().connect(
      [this]
      {
        if (!_syncingMembershipTag)
        {
          _membershipTagEdited = true;
        }

        updatePlaylistExpression();
      });
    _membershipTagRow =
      &detailsList->addEntryRow(gtkText(_textCatalog, i18n::MessageId::GtkSmartListMembershipTag), _membershipTagEntry);
    _membershipTagRow->set_visible(false);
    _leftPanel.append(*detailsList);

    auto* const filterList = Gtk::make_managed<FormBoxedList>();

    _inheritedExprLabel.set_halign(Gtk::Align::END);
    _inheritedExprLabel.set_ellipsize(Pango::EllipsizeMode::END);
    filterList->addRow(gtkText(_textCatalog, i18n::MessageId::GtkSmartListInheritedFilter), _inheritedExprLabel);

    _exprBox.entry().set_placeholder_text(
      gtkText(_textCatalog, i18n::MessageId::GtkSmartListFilterExpressionPlaceholder));
    _expressionChangedConnection = _exprBox.entry().signal_changed().connect(
      [this]
      {
        // Dismiss on the edit, not its delayed refresh: a write can fail before the timeout fires.
        dismissSubmissionError();
        _exprTimeoutConnection.disconnect();
        _exprTimeoutConnection = Glib::signal_timeout().connect(
          [this]
          {
            refreshDraftPreview();
            return false;
          },
          100);
      });
    filterList->addRow(gtkText(_textCatalog, i18n::MessageId::GtkSmartListLocalFilter), _exprBox);

    _effectiveExprLabel.set_halign(Gtk::Align::END);
    _effectiveExprLabel.set_ellipsize(Pango::EllipsizeMode::END);
    filterList->addRow(gtkText(_textCatalog, i18n::MessageId::GtkSmartListEffectiveFilter), _effectiveExprLabel);

    _membershipEditingLabel.set_halign(Gtk::Align::END);
    _membershipEditingLabel.set_wrap(true);
    filterList->addRow(gtkText(_textCatalog, i18n::MessageId::GtkSmartListMembership), _membershipEditingLabel);

    _leftPanel.append(*filterList);

    auto* const presList = Gtk::make_managed<FormBoxedList>();
    auto stringListPtr = Gtk::StringList::create();
    stringListPtr->append(gtkText(_textCatalog, i18n::MessageId::GtkSmartListAutoPresentation));

    for (auto const& preset : rt::builtinTrackPresentationPresets())
    {
      auto const optText = uimodel::builtinTrackPresentation(_textCatalog, preset.spec.id);
      stringListPtr->append(optText ? std::string{optText->label} : preset.spec.id);
    }

    // Custom presentations follow the builtins in the option list and keep
    // their user-provided names, so a list already assigned one reopens with
    // that presentation selected instead of silently falling back to Auto.
    for (auto const& preset : _customPresets)
    {
      stringListPtr->append(preset.label.empty() ? preset.spec.id : preset.label);
    }

    _presentationOptionsPtr = stringListPtr;
    _presentationDropDown.set_model(stringListPtr);

    // The selected option shares the fixed-width configuration pane with its
    // field label, so it must be able to give up natural width instead of
    // squeezing that label: its label ellipsizes, while the full option text
    // stays reachable through tooltip and accessible label. The popup keeps a
    // separate full-width factory, because the selected factory would
    // otherwise populate the popup as well.
    auto const selectedFactoryPtr = Gtk::SignalListItemFactory::create();
    selectedFactoryPtr->signal_setup().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
      {
        auto* const label = Gtk::make_managed<Gtk::Label>();
        label->set_ellipsize(Pango::EllipsizeMode::END);
        itemPtr->set_child(*label);
      });
    selectedFactoryPtr->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
      {
        auto* const label = dynamic_cast<Gtk::Label*>(itemPtr->get_child());

        if (label == nullptr)
        {
          return;
        }

        if (auto const stringPtr = std::dynamic_pointer_cast<Gtk::StringObject>(itemPtr->get_item()); stringPtr)
        {
          auto const text = stringPtr->get_string();
          label->set_text(text);
          setTooltipAndAccessibleLabel(*label, text.raw());
        }
      });

    auto const popupFactoryPtr = Gtk::SignalListItemFactory::create();
    popupFactoryPtr->signal_setup().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
      {
        // A popup row keeps its full option name leading and a trailing
        // check on the selected row, mirroring the native default content a
        // custom list factory would otherwise drop. The check is decorative,
        // so it leaves the accessibility tree.
        auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto* const label = Gtk::make_managed<Gtk::Label>();
        label->set_halign(Gtk::Align::START);
        label->set_hexpand(true);
        label->set_xalign(0.0F);
        auto* const check = Gtk::make_managed<Gtk::Image>();
        check->set_from_icon_name("object-select-symbolic");
        check->property_accessible_role() = Gtk::Accessible::Role::PRESENTATION;
        row->append(*label);
        row->append(*check);
        itemPtr->set_child(*row);

        // The check follows the row's selected state through a property
        // binding, like the native default content. The binding is kept alive
        // by its two endpoints and is released when the recycled list item or
        // its content is finalized, so this factory owns no signal and keeps
        // no binding reference.
        std::ignore = Glib::Binding::bind_property(
          itemPtr->property_selected(), check->property_visible(), Glib::Binding::Flags::SYNC_CREATE);
      });
    popupFactoryPtr->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
      {
        auto* const row = dynamic_cast<Gtk::Box*>(itemPtr->get_child());

        if (row == nullptr)
        {
          return;
        }

        if (auto* const label = dynamic_cast<Gtk::Label*>(row->get_first_child()); label != nullptr)
        {
          if (auto const stringPtr = std::dynamic_pointer_cast<Gtk::StringObject>(itemPtr->get_item()); stringPtr)
          {
            label->set_text(stringPtr->get_string());
          }
        }
      });

    _presentationDropDown.set_factory(selectedFactoryPtr);
    _presentationDropDown.set_list_factory(popupFactoryPtr);
    _presentationDropDown.set_valign(Gtk::Align::CENTER);
    _presentationDropDown.set_halign(Gtk::Align::END);
    presList->addRow(gtkText(_textCatalog, i18n::MessageId::GtkSmartListPresentation), _presentationDropDown);
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
    auto* const previewLabel =
      Gtk::make_managed<Gtk::Label>(gtkText(_textCatalog, i18n::MessageId::GtkSmartListPreview));
    previewLabel->add_css_class("ao-section-header");
    previewLabel->set_halign(Gtk::Align::START);
    previewHeaderBox->append(*previewLabel);

    _matchCountLabel.set_halign(Gtk::Align::END);
    _matchCountLabel.set_hexpand(true);
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

  void SmartListDialog::installUnavailablePresentationOption(std::string_view const unavailableId)
  {
    _optUnavailablePresentationId = std::string{unavailableId};
    _presentationOptionsPtr->append(i18n::requiredFormat(
      _textCatalog, i18n::MessageId::GtkSmartListUnavailablePresentation, {{"id", unavailableId}}));
    _presentationDropDown.set_selected(unavailablePresentationRow());
  }

  void SmartListDialog::removeUnavailablePresentationOption()
  {
    if (_optUnavailablePresentationId)
    {
      _presentationOptionsPtr->remove(unavailablePresentationRow());
      _optUnavailablePresentationId.reset();
    }
  }

  std::uint32_t SmartListDialog::unavailablePresentationRow() const
  {
    return 1U + static_cast<std::uint32_t>(rt::builtinTrackPresentationPresets().size()) +
           static_cast<std::uint32_t>(_customPresets.size());
  }

  void SmartListDialog::buildPreview()
  {
    configurePreviewColumns();
    _previewSetupConnection = Glib::signal_idle().connect(
      [this]
      {
        _previewModelPtr = TrackListModel::create(_trackRowCache);
        _previewSampleModelPtr = Gtk::SliceListModel::create(
          _previewModelPtr, 0U, static_cast<std::uint32_t>(uimodel::kSmartListPreviewLimit));
        _previewColumnView.set_model(Gtk::SingleSelection::create(_previewSampleModelPtr));
        refreshDraftPreview();
        return false;
      });
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
      [textCatalog = _textCatalog](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
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

          label->set_text(uimodel::formatSmartListPreviewTrackLabel(textCatalog, titleText, artistText, albumText));
        }
      });

    auto columnPtr =
      Gtk::ColumnViewColumn::create(gtkText(_textCatalog, i18n::MessageId::GtkSmartListTrack), factoryPtr);
    columnPtr->set_expand(true);
    columnPtr->set_resizable(true);
    _previewColumnView.append_column(columnPtr);
  }

  void SmartListDialog::updateSourceLabels()
  {
    auto inheritedExpr = std::string{};

    if (auto const isAllTracks = rt::isVirtualListId(_parentListId); !isAllTracks)
    {
      auto scope = _library.snapshot();

      if (auto optNode = scope.listNode(_parentListId); optNode)
      {
        inheritedExpr = optNode->expression;
      }
      else
      {
        _inheritedExprLabel.set_text(gtkText(_textCatalog, i18n::MessageId::GtkSmartListInvalidSource));
        _effectiveExprLabel.set_text(gtkText(_textCatalog, i18n::MessageId::GtkSmartListInvalidSource));
        return;
      }
    }

    _inheritedExprLabel.set_text(uimodel::formatSmartListExpressionDisplayText(_textCatalog, inheritedExpr));

    auto const localExpr = std::string{_exprBox.entry().get_text()};
    auto const effectiveExpression = ao::uimodel::combineSmartListEffectiveExpression(inheritedExpr, localExpr);
    _effectiveExprLabel.set_text(uimodel::formatSmartListExpressionDisplayText(_textCatalog, effectiveExpression));
  }

  void SmartListDialog::updatePlaylistExpression()
  {
    if (!_playlistTemplate)
    {
      return;
    }

    auto const tag = _membershipTagEntry.get_text().raw();
    auto expression = std::string{};

    if (!tag.empty())
    {
      expression = rt::serializeTagExpression(tag);
    }

    setLocalExpression(expression);
  }

  void SmartListDialog::updatePlaylistTagFromName()
  {
    if (!_playlistTemplate || _membershipTagEdited)
    {
      return;
    }

    _syncingMembershipTag = true;
    _membershipTagEntry.set_text(_nameEntry.get_text());
    _syncingMembershipTag = false;
  }

  uimodel::SmartListEditorViewState SmartListDialog::editorViewState() const
  {
    auto const name = _nameEntry.get_text();
    auto const localExpression = _exprBox.entry().get_text();
    auto const hasPreviewSource = _optPreviewSourceLease.has_value();
    auto const optError = hasPreviewSource ? _sources.sourceError(*_optPreviewSourceLease) : std::nullopt;
    auto const& errorMessage = optError ? optError->message : _previewAcquisitionError;
    auto const optSampleCount =
      _previewSampleModelPtr ? std::optional<std::size_t>{_previewSampleModelPtr->get_n_items()} : std::nullopt;

    return uimodel::makeSmartListEditorViewState(
      _textCatalog,
      uimodel::SmartListPreviewState{
        .name = name.raw(),
        .localExpression = localExpression.raw(),
        .hasPreviewSource = hasPreviewSource,
        .hasError = optError.has_value(),
        .errorMessage = errorMessage,
        .matchCount = hasPreviewSource ? _optPreviewSourceLease->source().size() : 0,
        .optSampleCount = optSampleCount,
        .isAllTracks = rt::isVirtualListId(_parentListId),
      });
  }

  void SmartListDialog::updateDialogState()
  {
    updateDialogState(editorViewState());
  }

  void SmartListDialog::updateDialogState(uimodel::SmartListEditorViewState const& state)
  {
    auto const playlistTagReady = !_playlistTemplate || !_membershipTagEntry.get_text().empty();
    _okButton->set_sensitive(state.canSubmit && playlistTagReady && !_submissionPending);
    _membershipEditingLabel.set_text(playlistTagReady
                                       ? state.membershipEditingText
                                       : gtkText(_textCatalog, i18n::MessageId::GtkSmartListChooseMembershipTag));
  }

  void SmartListDialog::updateErrorLabel(uimodel::SmartListEditorViewState const& state)
  {
    auto const text = uimodel::formatSmartListEditorErrorText(_submissionError, state);
    _errorLabel.set_text(text);
    _errorLabel.set_visible(!text.empty());
  }

  void SmartListDialog::acquirePreviewSource()
  {
    _previewRefreshConnection.disconnect();
    _previewSourceSubscription.reset();
    _previewModelPtr->clearProjection();

    auto const expr = std::string{_exprBox.entry().get_text()};
    auto const sourceListId = rt::resolveParentSourceId(_parentListId);
    auto sourceRes = _sources.acquire(rt::SourceSpec{.baseListId = sourceListId, .filterExpression = expr});

    if (!sourceRes)
    {
      APP_LOG_ERROR("Cannot build smart-list preview for source {}: {}", sourceListId, sourceRes.error().message);
      _optPreviewSourceLease.reset();
      _previewAcquisitionError = std::move(sourceRes.error().message);
      return;
    }

    _optPreviewSourceLease.emplace(std::move(*sourceRes));
    _previewAcquisitionError.clear();
    _previewSourceSubscription = _optPreviewSourceLease->source().subscribe(
      [this](rt::TrackSourceDelta const&)
      {
        // Changes past the end of the GTK slice still affect the total.
        // Wait until both projection updates and source-cache retirement finish.
        if (!_previewRefreshConnection.connected())
        {
          _previewRefreshConnection = Glib::signal_idle().connect(
            [this]
            {
              if (!_exprTimeoutConnection.connected())
              {
                refreshPreview();
              }

              return false;
            });
        }
      });
  }

  void SmartListDialog::dismissSubmissionError()
  {
    if (!_submissionError.empty())
    {
      _submissionError.clear();
      updateErrorLabel(editorViewState());
    }
  }

  void SmartListDialog::refreshDraftPreview()
  {
    _exprTimeoutConnection.disconnect();

    if (_previewModelPtr)
    {
      acquirePreviewSource();
    }

    refreshPreview();
  }

  void SmartListDialog::refreshPreview()
  {
    updateSourceLabels();

    if (_optPreviewSourceLease && _optPreviewSourceLease->source().state() == rt::TrackSourceState::Invalidated)
    {
      acquirePreviewSource();
    }

    if (_optPreviewSourceLease)
    {
      auto const localExpression = _exprBox.entry().get_text();
      auto const optError = _sources.sourceError(*_optPreviewSourceLease);

      if (uimodel::isSmartListLocalFilterRejected(localExpression.raw(), optError.has_value()))
      {
        _previewModelPtr->clearProjection();
      }
      else if (_previewModelPtr->projection() == nullptr)
      {
        _previewModelPtr->bindProjection(
          _views.createTransientTrackListProjection(*_optPreviewSourceLease, rt::TrackOrderSpec{}));
      }
    }

    auto const state = editorViewState();
    _matchCountLabel.set_markup(italicMarkup(state.previewStatusText));

    if (state.queryInvalid)
    {
      _exprBox.entry().add_css_class("ao-query-invalid");
    }
    else
    {
      _exprBox.entry().remove_css_class("ao-query-invalid");
    }

    updateErrorLabel(state);
    _previewScrolledWindow.set_visible(state.previewVisible);
    updateDialogState(state);
  }

  rt::ListDraft SmartListDialog::draft() const
  {
    return ao::uimodel::makeSmartListDraft(
      _parentListId, _editListId, _nameEntry.get_text(), _descEntry.get_text(), _exprBox.entry().get_text());
  }
} // namespace ao::gtk
