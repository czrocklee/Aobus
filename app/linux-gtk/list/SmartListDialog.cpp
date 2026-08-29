// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/SmartListDialog.h"

#include "app/FormBuilder.h"
#include "i18n/GtkText.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/list/SmartListEditing.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <glibmm/main.h>
#include <glibmm/markup.h>
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

#include <cstddef>
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
      return std::format("<i>{}</i>", Glib::Markup::escape_text(std::string{text}).raw());
    }
  }

  SmartListDialog::SmartListDialog(Gtk::Window& parent,
                                   rt::AppRuntime& runtime,
                                   i18n::MessageCatalog textCatalog,
                                   ListId parentListId,
                                   TrackRowCache const& provider)
    : _exprBox{runtime.completion(), textCatalog}
    , _runtime{runtime}
    , _textCatalog{textCatalog}
    , _parentListId{parentListId}
    , _trackRowCache{provider}
  {
    set_title(gtkText(_textCatalog, i18n::MessageId::GtkSmartListNewTitle));
    configureForParent(parent);
    buildUi();
    buildPreview();
    updatePreview();
  }

  SmartListDialog::~SmartListDialog()
  {
    _presentationCallbacks.close();
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
    _exprBox.entry().set_text(node.expression);
    set_title(gtkText(_textCatalog, i18n::MessageId::GtkSmartListEditTitle));
    _okButton->set_label(gtkText(_textCatalog, i18n::MessageId::GtkCommonSave));

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

  void SmartListDialog::configurePlaylistTemplate(std::string_view const initialName, std::string_view const initialTag)
  {
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
    _exprTimeoutConnection.disconnect();
    _exprBox.entry().set_text(std::string{expression});
    updatePreview();
  }

  void SmartListDialog::showError(std::string_view message)
  {
    _errorLabel.set_text(std::string{message});
    _errorLabel.set_visible(!message.empty());
  }

  bool SmartListDialog::beginSubmission()
  {
    if (std::exchange(_submissionPending, true))
    {
      return false;
    }

    updateDialogState();
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
    _nameEntry.signal_changed().connect(
      [this]
      {
        updatePlaylistTagFromName();
        updateDialogState();
      });
    detailsList->addEntryRow(gtkText(_textCatalog, i18n::MessageId::GtkSmartListName), _nameEntry);

    _descEntry.set_placeholder_text(gtkText(_textCatalog, i18n::MessageId::GtkSmartListDescriptionPlaceholder));
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

    _presentationDropDown.set_model(stringListPtr);
    _presentationDropDown.set_valign(Gtk::Align::CENTER);
    _presentationDropDown.set_halign(Gtk::Align::END);
    _presentationDropDown.property_selected().signal_changed().connect([this] { updatePreview(); });
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
    _matchCountLabel.set_markup(
      std::format("<span alpha='50%'>{}</span>",
                  italicMarkup(gtkText(_textCatalog, i18n::MessageId::GtkSmartListWaitingForFilter))));
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
      [this](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
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

          label->set_text(uimodel::formatSmartListPreviewTrackLabel(_textCatalog, titleText, artistText, albumText));
        }
      });

    auto columnPtr =
      Gtk::ColumnViewColumn::create(gtkText(_textCatalog, i18n::MessageId::GtkSmartListTrack), factoryPtr);
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

        _optPreviewSourceLease.reset();
        _previewModelPtr.reset();

        _previewModelPtr = TrackListModel::create(_trackRowCache);

        auto selectionModelPtr = Gtk::SingleSelection::create(_previewModelPtr);
        _previewColumnView.set_model(selectionModelPtr);

        // Re-run the full preview so the model can replace its
        // source-unavailable state as soon as the source arrives.
        updatePreview();

        return false;
      });
  }

  void SmartListDialog::updateSourceLabels()
  {
    auto inheritedExpr = std::string{};

    if (auto const isAllTracks = rt::isVirtualListId(_parentListId); !isAllTracks)
    {
      auto scope = _runtime.library().snapshot();

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
      expression = query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = tag});
    }

    _exprTimeoutConnection.disconnect();
    _exprBox.entry().set_text(expression);
    updatePreview();
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
    auto const hasPreviewSource = _optPreviewSourceLease.has_value();
    auto const optError = hasPreviewSource ? _runtime.sources().sourceError(*_optPreviewSourceLease) : std::nullopt;

    return ao::uimodel::makeSmartListEditorViewState(
      _textCatalog,
      ao::uimodel::SmartListPreviewState{
        .name = _nameEntry.get_text().raw(),
        .localExpression = _exprBox.entry().get_text().raw(),
        .hasPreviewSource = hasPreviewSource,
        .hasError = optError.has_value(),
        .errorMessage = optError ? optError->message : std::string{},
        .matchCount = hasPreviewSource ? _optPreviewSourceLease->source().size() : 0,
        .isAllTracks = rt::isVirtualListId(_parentListId),
      });
  }

  void SmartListDialog::updateDialogState()
  {
    auto const state = editorViewState();
    auto const playlistTagReady = !_playlistTemplate || !_membershipTagEntry.get_text().empty();
    _okButton->set_sensitive(state.canSubmit && playlistTagReady && !_submissionPending);
    _membershipEditingLabel.set_text(playlistTagReady
                                       ? state.membershipEditingText
                                       : gtkText(_textCatalog, i18n::MessageId::GtkSmartListChooseMembershipTag));
  }

  void SmartListDialog::updatePreview()
  {
    updateSourceLabels();

    if (!_previewModelPtr)
    {
      _previewScrolledWindow.set_visible(false);
      updateDialogState();
      return;
    }

    auto const expr = std::string{_exprBox.entry().get_text()};
    auto const sourceListId = rt::resolveParentSourceId(_parentListId);
    auto sourceRes = _runtime.sources().acquire(rt::SourceSpec{.baseListId = sourceListId, .filterExpression = expr});

    if (!sourceRes)
    {
      APP_LOG_ERROR("Cannot build smart-list preview for source {}: {}", sourceListId, sourceRes.error().message);
      _optPreviewSourceLease.reset();
      _previewModelPtr->clearProjection();
      _previewScrolledWindow.set_visible(false);
      _matchCountLabel.set_markup(italicMarkup(gtkText(_textCatalog, i18n::MessageId::GtkSmartListWaitingForFilter)));
      showError(sourceRes.error().message);
      updateDialogState();
      return;
    }

    _optPreviewSourceLease.emplace(std::move(*sourceRes));
    _previewModelPtr->bindProjection(
      _runtime.views().createTransientTrackListProjection(*_optPreviewSourceLease, rt::TrackOrderSpec{}));

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

    _errorLabel.set_visible(state.errorVisible);
    _errorLabel.set_text(state.errorText);
    _previewScrolledWindow.set_visible(state.previewVisible);
    auto const playlistTagReady = !_playlistTemplate || !_membershipTagEntry.get_text().empty();
    _membershipEditingLabel.set_text(playlistTagReady
                                       ? state.membershipEditingText
                                       : gtkText(_textCatalog, i18n::MessageId::GtkSmartListChooseMembershipTag));

    _okButton->set_sensitive(state.canSubmit && playlistTagReady && !_submissionPending);
  }

  rt::ListDraft SmartListDialog::draft() const
  {
    return ao::uimodel::makeSmartListDraft(
      _parentListId, _editListId, _nameEntry.get_text(), _descEntry.get_text(), _exprBox.entry().get_text());
  }
} // namespace ao::gtk
